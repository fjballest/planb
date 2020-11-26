#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <auth.h>
#include <draw.h>
#include <mouse.h>
#include <frame.h>
#include <9p.h>
#include <pool.h>
#include <b.h>
#include "gui.h"

int mainstacksize = 32 * 1024;


File*	slash;
char*	sname;
char*	saddr;
int	smallomero;
char*	remoteox;
static Channel*	fsreqc;
static Channel* writeallc;
static Channel* pidc;
Channel*	startc;
Channel*	panelhupc;
Channel* 	destroyc;
static char*	vname;
static char*	addr;
static ulong	oxpids[4];

static void
fsattach(Req* r)
{
	if (r->srv->auth != nil && authattach(r) < 0)
		return;
	respond(r, nil);
}

int
fileclass(File* f)
{
	return ((f->qid.path&0xff000000)>>24);
}

void
setfileclass(File* f, int id)
{
	f->qid.path |= (((id)&0xff)<<24);
}

File*
newfspanel(File* d, char* name, char* uid)
{
	File*	f;
	Panel*	p;
	int	id;
	Panel*	dp;

	cleanpolicy(d);
	dp = d->aux;
	p = newpanel(name, dp);
	if (p == nil)
		return nil;
	id = p->type;
	f =  createfile(d, name, uid, 0775|DMDIR, nil);
	if (f == nil){
		closepanel(p);
		fprint(2, "omero: newfspanel: %s: %r\n", name);
		return nil;
	}
	setfileclass(f, id*2);
	p->path = filepath(f);
	p->file = f;
	p->dfile = createfile(f, "data", uid, 0660,   p);
	incref(p);
	setfileclass(p->dfile, id*2);
	closefile(p->dfile);
	p->cfile = createfile(f, "ctl",  uid, 0660, p);
	incref(p);
	setfileclass(p->cfile, id*2+1);
	closefile(p->cfile);
	setctlfilelen(p);
	f->aux = p;
	p->flags |= Predraw;
	dp->flags|= Predraw;
	return f;
}

static void
fsopen(Req* r)
{
	File*	f;
	int	type;
	Panel*	p;
	int	mode;

	if (r->fid->qid.type&QTAUTH)
		sysfatal("fsopen for an AUTH file. fixme: add respond/return\n");
	mode = (r->ifcall.mode&3);
	if (mode == OREAD){
		respond(r, nil);
		return;
	}
	f = r->fid->file;
	type = fileclass(f);
	p = f->aux;
	panelok(p);
	if ((type&1) == 0 && (r->ifcall.mode&OTRUNC) && panels[p->type]->truncate){
		wlock(p->dfile);
		panels[p->type]->truncate(p);
		wunlock(p->dfile);
	}
	if ((f->qid.type&QTDIR) || (type&1) || panels[p->type]->writeall == nil){
		respond(r, nil);
		return;
	}
	/*
	 * Trying to open a panel data file that needs to process all
	 * the writes at onces (e.g., images: created once copied).
	 * Make sure the file behaves as OEXCL regarding writes.
	 */
	wlock(f);
	if (p->writebuf != nil){
		wunlock(f);
		respond(r, "exclusive use for write");
	} else {
		p->writebuf = emalloc9p(64*1024);
		p->nawrite = 64*1024;
		p->nwrite = 0;
		wunlock(f);
		respond(r, nil);
	}
}

static int
isheld(File* f)
{
	Panel*	p;

	for(;;){
		p = f->aux;
		if (p->flags&Ptop)
			break;
		if (p->flags&Playout)
			break;
		if (f == f->parent)
			break;
		if (p->flags&Phold)
			return 1;
		f = f->parent;
	}
	return 0;
}

static void
fscreate(Req* r)
{
	File*	f;
	File*	d;
	char*	name;
	char*	uid;
	int	mode;
	Panel*	p;

	d = r->fid->file;
	name = r->ifcall.name;
	uid = r->fid->uid;
	mode = d->mode & 0x777 & r->ifcall.perm;
	mode |= (r->ifcall.perm & ~0x777);
	if (!(mode&DMDIR)){
		werrstr("%s must be a directory", name);
		responderror(r);
		return;
	}
	p = d->aux;
	if (p->type != Qcol && p->type != Qrow){
		werrstr("not a column or a row: type %d", p->type);
		responderror(r);
		return;
	}
	if (f = newfspanel(d, name, uid)){
		closefile(r->fid->file);
		r->fid->file = f;
		r->ofcall.qid = f->qid;
		p->flags |= Predraw;
		if (!isheld(f))
			resize();
		respond(r, nil);
	} else
		responderror(r);
}

static void
fsread(Req* r)
{
	File*	f;
	Panel*	cp;
	int	type;
	char	buf[512];
	long	n;
	void*	data;
	long	count;
	vlong	off;

	if (r->fid->qid.type&QTAUTH){
		authread(r);
		return;
	}
	f = r->fid->file;
	type = fileclass(f);
	cp = f->aux;
	assert(cp);
	if (cp->dfile == nil || cp->cfile == nil || cp->file == nil || (cp->flags&Pdead)){
		respond(r, "panel is deleted");
		return;
	}
	panelok(cp);
	if (type&1){
		rlock(cp->dfile);
		n = panels[cp->type]->attrs(cp, buf, sizeof(buf)-1);
		runlock(cp->dfile);
		if (n < 0){
			responderror(r);
			return;
		}
		buf[n] = 0;
		readstr(r, buf);
	} else {
		data = r->ofcall.data;
		off = r->ifcall.offset;
		count = r->ifcall.count;
		rlock(cp->dfile);
		n = panels[cp->type]->read(cp, data, count, off);
		runlock(cp->dfile);
		if (n < 0){
			responderror(r);
			return;
		}
		r->ofcall.count = n;
	}
	respond(r, nil);
}

static int
multilinectl(char* c)
{
	if(!strncmp(c, "ins ", 4) || !strncmp(c, "search ", 7))
		return 1;
	return 0;
}

static void
fswrite(Req* r)
{
	File*	f;
	Panel*	cp;
	int	type, n;
	void*	data;
	long	count;
	vlong	off;
	char*	buf;
	char*	s;
	char*	e;

	if (r->fid->qid.type&QTAUTH){
		authwrite(r);
		return;
	}
	if (r->ifcall.count == 0){
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}
	f = r->fid->file;
	type = fileclass(f);
	cp = f->aux;
	assert(cp);
	if (cp->dfile == nil || cp->cfile == nil || cp->file == nil){
		respond(r, "panel is deleted");
		return;
	}
	panelok(cp);
	if (type&1){
		buf = emalloc9p(r->ifcall.count + 1);
		memmove(buf, r->ifcall.data, r->ifcall.count);
		buf[r->ifcall.count] = 0;
		r->ofcall.count = r->ifcall.count;
		for (s = buf; s && *s; s = e){
			if (multilinectl(s)){	// consume all buf
				e = nil;
				if (buf[r->ifcall.count-1] == '\n')
					buf[r->ifcall.count-1] = 0;
			} else {
				e = strchr(s, '\n');
				if (e)
					*e++ = 0;
			}
			if (*s){
				wlock(cp->dfile);
				if (!strcmp(s, "hold")){
					cp->holdfid = r->fid->fid;
					cp->flags |= Phold;
				} else if (panels[cp->type]->ctl(cp, s) < 0){
					wunlock(cp->dfile);
					free(buf);
					responderror(r);
					return;
				}
				wunlock(cp->dfile);
			}
		}
		free(buf);
	} else {
		data = r->ifcall.data;
		off = r->ifcall.offset;
		count = r->ifcall.count;
		wlock(cp->dfile);
		if (panels[cp->type]->writeall != nil){
			if (off + count > cp->nawrite){
				cp->writebuf = erealloc9p(cp->writebuf, off+count);
				cp->nawrite = off+count;
			}
			memmove(cp->writebuf+off, data, count);
			n = count;
			cp->nwrite = off+count;
		} else
			n = panels[cp->type]->write(cp, data, count, off);
		wunlock(cp->dfile);
		if (n < 0){
			responderror(r);
			return;
		}
		r->ofcall.count = n;
	}
	respond(r, nil);
}

static void
fsclunk(Fid* fid)
{
	File*	f;
	Panel*	cp;
	int	type;

	if (fid->qid.type&QTAUTH){
		authdestroy(fid);
		return;
	}

	f = fid->file;
	if (f == nil)
		return;
	type = fileclass(f);
	cp = f->aux;
	assert(cp);
	if (fid->fid == cp->holdfid){
		cp->holdfid = 0;
		cp->flags &= ~Phold;
		resize();
		edprint("hold clear\n");
	}
	if (!(fid->qid.type&QTDIR) && !(type&1) && panels[cp->type]->writeall)
	if (cp->dfile != nil && cp->cfile != nil && cp->file != nil)
		sendp(writeallc, cp);
}

static void
fsremove(Req* r)
{
	File*	fp;
	Panel*	p;

	// Be sure the parent is redrawn later.
	// freefile() will actually remove the file when it's safe.
	fp = r->fid->file;
	p = fp->aux;
	assert(p);
	p->flags |= Pdead;
	if (fp != nil && fp->parent != nil){
		p = fp->parent->aux;
		p->flags |= Predraw;
	}
	respond(r, nil);
}

static void
freefile(File *f)
{
	Panel*	p;

	p = f->aux;
	f->aux = nil;
	p->flags |= Pdead;
	if (p->dfile == f)
		p->dfile = nil;
	else if (p->cfile == f)
		p->cfile = nil;
	else if (p->file == f)
		p->file = nil;
	if (decref(p) <= 0)
		sendp(destroyc, p);
}


static void
fsinit(Srv* s)
{
	assert(!s->aux);
	s->aux = chancreate(sizeof(ulong), 0);
}

static void
fsend(Srv* s)
{
	assert(s->aux);
	chanfree(s->aux);
}

static void
fsreqthread(void* a)
{
	Req*	r = a;

	threadsetname("fsreqthread");
	switch(r->ifcall.type){
	case Topen:
		fsopen(r);
		break;
	case Tcreate:
		fscreate(r);
		break;
	case Tremove:
		fsremove(r);
		break;
	case Tread:
		fsread(r);
		break;
	case Twrite:
		fswrite(r);
		break;
	default:
		sysfatal("bad request in fsthread type %d\n", r->ifcall.type);
	}
	threadexits(nil);

}

static void
fsthread(void*)
{
	int	i;
	Req*	r;
	Con*	c;
	Panel*	p;
	Waitmsg*wm;
	Alt	a[] = {
		{fsreqc, &r, CHANRCV},
		{writeallc, &p, CHANRCV},
		{nil, &wm, CHANRCV},
		{panelhupc, &c, CHANRCV},
		{nil, nil, CHANEND}};

	threadsetname("fsthread");
	a[2].c = threadwaitchan();
	for(;;){
		switch(alt(a)){
		case 0:
			threadcreate(fsreqthread, r, 32*1024);
			break;
		case 1:
			if (p->dfile == nil || p->cfile == nil || p->file == nil || (p->flags&Pdead))
				break;
			panelok(p);
			wlock(p->dfile);
			panels[p->type]->writeall(p, p->writebuf, p->nwrite);
			free(p->writebuf);
			p->writebuf = nil;
			p->nawrite = p->nwrite = 0;
			wunlock(p->dfile);
			break;
		case 2:
			edprint("pid %d exited\n", wm->pid);
			for (i = 0; i < 4; i++)
				if (oxpids[i] == wm->pid)
					oxpids[i] = 0;
			free(wm);
			break;
		case 3:
			rmhuppanels(slash, c);
			break;
		default:
			abort();
		}
	}
		
}

static int
islocal(char* addr)
{
	static char*	l = nil;
	if (addr == nil)
		return 1;
	if (!strncmp(addr, "::", 2) || !strncmp(addr, "127.0.0.1", 8))
		return 1;
	if (l == nil)
		l = getenv("sysaddr");
	if (l && !strncmp(addr, l, strlen(l)))
		return 1;
	return 0;
}

static void
bauth9p(Req* r)
{
	if (islocal(r->srv->addr)) {
		r->srv->auth = nil;	// no auth here any more
		respond(r, "auth no required for local peers");
	} else
		auth9p(r);
}


static void
fsop(Req* r)
{
	sendp(fsreqc, r);
}

static Srv sfs=
{
	.auth	= bauth9p,
	.attach	= fsattach,
	.open	= fsop,
	.create	= fsop,
	.remove	= fsop,
	.read	= fsop,
	.write	= fsop,
	.destroyfid = 	fsclunk,
};

static char* menu[] = { "Row", "Col", "Ox", "Del" };

static void
mklayout(File* f)
{
	File*	d;
	File*	dd;
	File*	c;
	Panel*	p;
	int	i;
	char	name[50];

	d = newfspanel(f, "row:stats", f->uid);
	p = d->aux;
	p->flags |= Ptag|Playout;
	if (smallomero)
		dd = newfspanel(d, "col:cmds", f->uid);
	else
		dd = newfspanel(d, "row:cmds", f->uid);
	for (i = 0; i < nelem(menu); i++){
		seprint(name, name+sizeof(name), "button:%s", menu[i]);
		c = newfspanel(dd, name, f->uid);
		closefile(c);
	}
	closefile(dd);
	closefile(d);

	d = newfspanel(f, "row:wins", f->uid);
	p = dd->aux;
	p->flags |= Playout;
	dd = newfspanel(d, "col:1", f->uid);
	p = dd->aux;
	p->flags |= Playout;
	closefile(dd);
	if (!smallomero){
		dd = newfspanel(d, "col:2", f->uid);
		p = dd->aux;
		p->flags |= Playout;
		closefile(dd);
	}
	closefile(d);
	resize();
}

static void
mktree(void)
{
	Panel*	p;
	File*	d;

	sfs.tree = alloctree(nil, nil, DMDIR|0555, freefile);
	if (sfs.tree == nil)
		sysfatal("no tree: %r");
	slash = sfs.tree->root;
	setfileclass(slash, 2*Qcol);
	incref(slash);
	slash->aux = p = newpanel("col:/", nil);
	p->file = slash;
	p->path = filepath(slash);
	p->flags &= ~Ptag;
	p->flags |= Ptop|Playout;

	p = newpanel("col:sys", p);
	p->flags |= Ptag|Ptop|Playout;
	d = createfile(slash, smprint("%sui", sname), "sys", DMDIR|0775, p);
	setfileclass(d, 2*Qcol);
	p->file = d;
	p->path = filepath(d);
	p->dfile = createfile(d, "data", "sys", 0660, p);
	incref(p);
	setfileclass(p->dfile, 2*Qcol);
	closefile(p->dfile);
	p->cfile = createfile(d, "ctl",  "sys", 0660, p);
	incref(p);
	setfileclass(p->cfile, 2*Qcol+1);
	closefile(p->cfile);
	mklayout(d);
}

static void
mountuis(void)
{
	int	fd;
	char	spec[50];
	char*	loc;

	loc = getenv("location");
	if (loc == nil)
		loc=strdup("home");
	seprint(spec, spec+50, "*/devs/ui 'user=%s loc=%s'", getuser(), loc);
	free(loc);
	fd = open("/srv/vol", ORDWR);
	if (fd < 0)
		fd = open("#s/vol", ORDWR);
	if (fd < 0)
		fprint(2, "%s: /srv/vol: %r\n", argv0);
	else {
		if (mount(fd, -1, "/devs", MBEFORE|MCREATE, spec) < 0)
			fprint(2, "%s: mount */devs/ui: %r\n", argv0);
		close(fd);
	}
}

void
startproc(void*a)
{
	char*	init = a;
	int	i;
	int	fd;
	char*	s;
	char*	r;

	for (i = 3; i < 30; i++)
		close(i);
	fd = open("/dev/null", OREAD);
	dup(fd, 0);
	close(fd);
	s = strchr(init, ' ');
	r = nil;
	if (s){
		*s++ = 0;
		r = strchr(s, ' ');
		if (r)
			*r++ = 0;
	}
	mountuis();
	procexecl(pidc, init, init, s, r, nil);
	procexecl(pidc, smprint("/bin/%s", init), init, s, r, nil);
	fprint(2, "%s: %s: %r\n", argv0, init);
	sendul(pidc, 0);
	threadexits("init");
}

void
runoxloop(char* p)
{
	char*	prog;
	char*	remote;
	char	buf[100];
	int	i;

	pidc = chancreate(sizeof(ulong), 0);
	procrfork(startproc, Ox, 16*1024, RFNAMEG|RFENVG|RFNOTEG);
	oxpids[0] = recvul(pidc);
	if (p != nil){
		proccreate(startproc, p, 16*1024);
		recvul(pidc);
	}
	while(prog=recvp(startc)){
		for (i = 0; i < 4; i++)
			if (oxpids[i] == 0)
				break;
		if (i == 4){
			fprint(2, "%s: no more oxs\n", argv0);
			continue;
		}
		remote = strchr(prog, ' ');
		if (remote){
			*remote++ = 0;
			fprint(2, "remote %s for %s not implemented\n", prog, remote);
			/* To implement this beast:
			 *  1. dial to the remote machine's ox port.
			 *  2. write params: ox prog, space id, our vols
			 *  3. block reading (this would exit when ox dies).
			 * To implement the remote ox program:
			 *  1. read parms.
			 *  2. adjust name space to use terminal device vols
			 *  3. exec ox
			 */
		} else {
			seprint(buf, buf+sizeof(buf), "%s -s%d", prog, i);
			procrfork(startproc, buf, 16*1024, RFNAMEG|RFENVG|RFNOTEG);
			oxpids[i] = recvul(pidc);
		}
		free(prog);
	}
}

static void
announceproc(void*)
{
	int	afd = -1;
	char*	cnstr;


	threadsetname("announceproc");
	cnstr = strchr(vname, ' ');
	if (cnstr)
		*cnstr++ = 0;

	for(;;){
		afd = announcevol(afd, addr, vname, cnstr);
		if (afd < 0) // try again
			afd = announcevol(afd, addr, vname, cnstr);
		sleep(10 * 1000);
	}
}

static char*
getvolsys(char* vname)
{
	char*	s;
	char*	p;
	char*	e;

	s = strdup(vname);
	e = strstr(s, "sys=");
	if (e == nil)
		return strdup(sysname());
	e += 4;
	p = strchr(e, ' ');
	if (p)
		*p = 0;
	p = strdup(e);
	free(s);
	return p;
}

static char*
cleanvoladdr(char* addr)
{
	char	naddr[50];
	char*	p;

	p = strchr(addr, '*');
	if (p == nil)
		return strdup(addr);
	else {
		if (p - addr)
			strncpy(naddr, addr, p - addr);
		naddr[p-addr] = 0;
		strcat(naddr, sysname());
		strcat(naddr, p + 1);
		return strdup(naddr);
	}
}

static void
mainproc(void*)
{
	/* We are the process servicing /devs/*ui, avoid deadlocks
	 */
	unmount(0, "/devs/");
	threadsetname("main");
	threadcreate(fsthread, nil, 32*1024);
	initui(); 
	mktree();
	rendezvous(mainproc, 0);
	threadexits(nil);
}

/* BUG:
 * The next few routines are similar to lib9p/thread.c and lib9p/listen.c
 * They are modified here to try tcp!*!0 if the user-given address fails,
 * to simplify calling for extra omeros. If a better way is found, this may go.
 */

static char*
getremotesys(char *ndir)
{
	char buf[128], *serv, *sys;
	int fd, n;

	snprint(buf, sizeof buf, "%s/remote", ndir);
	sys = nil;
	fd = open(buf, OREAD);
	if(fd >= 0){
		n = read(fd, buf, sizeof(buf)-1);
		if(n>0){
			buf[n-1] = 0;
			serv = strchr(buf, '!');
			if(serv)
				*serv = 0;
			sys = estrdup9p(buf);
		}
		close(fd);
	}
	if(sys == nil)
		sys = estrdup9p("unknown");
	return sys;
}

static void
osrvproc(void* v)
{
	int data;
	Srv *s;

	s = v;
	data = s->infd;
	if (chatty9p)
		fprint(2, "%d %s: new srv: %s\n", getpid(), argv0, s->addr);
	srv(s);
	if (chatty9p)
		fprint(2, "%d %s: exiting: %s\n", getpid(), argv0, s->addr);
	close(data);
	free(s->addr);
	free(s);
}

static void
olistenproc(void *v)
{
	char ndir[NETPATHLEN], dir[NETPATHLEN];
	int ctl, data, nctl;
	Srv *os, *s;
	NetConnInfo*	ni;	// I love these names that break indent
	
	os = v;
	ctl = announce(os->addr, dir);
	if(ctl < 0){
		ctl = announce("tcp!*!0", dir);
		if (ctl < 0)
			sysfatal("announce %s: %r", os->addr);
	}
	ni = getnetconninfo(dir, ctl);
	addr = smprint("tcp!%s!%s", sysname(), ni->lserv);
	saddr = addr;
	freenetconninfo(ni);
	rendezvous(olistenproc, 0);
	for(;;){
		nctl = listen(dir, ndir);
		if(nctl < 0){
			fprint(2, "%s: listen %s: %r", argv0, os->addr);
			break;
		}
		
		data = accept(ctl, ndir);
		if(data < 0){
			fprint(2, "%s: accept %s: %r\n", argv0, ndir);
			continue;
		}

		s = emalloc9p(sizeof *s);
		*s = *os;
		s->addr = getremotesys(ndir);
		s->infd = s->outfd = data;
		s->fpool = nil;
		s->rpool = nil;
		s->rbuf = nil;
		s->wbuf = nil;
		proccreate(osrvproc, s, 32*1024);
	}
	free(os->addr);
	free(os);
}

void
othreadlistensrv(Srv *os, char *addr)
{
	Srv *s;

	s = emalloc9p(sizeof *s);
	*s = *os;
	s->addr = estrdup9p(addr);
	proccreate(olistenproc, s, 32*1024);
	rendezvous(olistenproc, 0);
}

void
usage(void)
{
	/* Nasty. We should remove most options. Now we know how we use this
	 * and what does not make sense.
	 */
	fprint(2, "usage: %s [-A] [-dDCFLBMTS] [-p] [-n addr] [-V vol] [initprog]\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char **argv)
{
	char*	srv;
	int	mflag;
	char*	mnt;
	char*	l;
	int	slow;
	char*	prog;

	srv = nil;
	mnt = nil;
	addr = nil;
	mflag = (MBEFORE|MCREATE);
	slow = 0;
	ARGBEGIN{
	case 'A':
		sfs.auth = nil;
		break;
	case 'D':
		chatty9p++;
		break;
	case 'C':
		condebug++;
		break;
	case 'F':
		framedebug++;
		break;
	case 'L':
		layoutdebug++;
		break;
	case 'B':
		blockdebug++;
		break;
	case 'E':
	case 'd':
		eventdebug++;
		break;
	case 'M':
		mainmem->flags |= POOL_PARANOIA;
		break;
	case 'T':
		textdebug++;
		break;
	case 'S':
		slow++;
		slow9p = slow * 100;
		break;
	case 'p':
		smallomero++; 
		break;
	case 't':
		setterm(EARGF(usage()));
		break;
	case 'n':
		addr = EARGF(usage());
		break;
	case 'V':
		vname = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;
	prog = nil;
	if (argc >= 1){
		prog = argv[0];
		argc--;
		argv++;
	}
	while(argc > 0){
		setterm(argv[0]);
		argc--;
		argv++;
	}

	if (l = getenv("local")){
		if (!strcmp(l, "yes"))
			sfs.auth = nil;
		free(l);
	}
	if (vname != nil)
		sname = getvolsys(vname);
	else {
		vname = strdup("/devs/ui");
		sname = sysname();
	}
	if (sname == nil)
		sysfatal("no screen name");
	if (srv == nil && mnt == nil)
		mnt = "/devs";
	if (addr == nil)
		addr= "tcp!*!11007";
	if (addr != nil)
		saddr = cleanvoladdr(addr);

	rfork(RFENVG);
	if (!chatty9p)
		rfork(RFNOTEG);
fprint(2, "vname %s sname %s mnt %s addr %s\n", vname, sname, mnt, saddr);
	fsreqc = chancreate(sizeof(Req*), 16);
	destroyc = chancreate(sizeof(Panel*), 16);
	startc = chancreate(sizeof(char*), 0);
	writeallc = chancreate(sizeof(Panel*), 0);
	panelhupc = chancreate(sizeof(Con*), 0);
	procrfork(mainproc, 0, 16*1024, RFNAMEG);
	rendezvous(mainproc, 0);
	if (addr != nil){
		othreadlistensrv(&sfs, addr);
		if (vname == nil && !strcmp(addr, "tcp!*!11007"))
			vname = strdup("/devs/ui");
	}
	dprint("vname %s srv %s mnt %s sname %s addr %s\n", vname, srv, mnt,sname, saddr);
	if (srv != nil || mnt != nil)
		threadpostmountsrv(&sfs, srv, mnt, mflag);
	if (addr != nil && vname != nil)
		proccreate(announceproc, 0, 8*1024);
	putenv("omero", smprint("%s/%sui/row:wins/col:1", mnt, sname));
	if (mnt != nil)
		runoxloop(prog);
}
