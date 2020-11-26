#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <auth.h>
#include <9p.h>
#include <b.h>

typedef struct Port	Port;
typedef struct Reader	Reader;

enum {
	Stack	= 16 * 1024,
	Nreader	= 32,
	Nports	= 128,
};

struct Reader {
	Fid*	fid;	// fid used by this reader.
	Req*	r;	// outstanding rd request (at most one) or nil.
	char*	data;	// data from messages avail for this reader.
	int	ndata;	// how much data.
	int	rdoff;
};

struct Port{
	Reader	rd[Nreader];	// pending read requests
	int	orclose;	// if true, remove on last close.
	int	nopens;
};

enum {
	Qport,
};

Lock	portlck;
Port*	port[Nports];
int	nports;
char*	addr;
char*	vname = "/mnt/plumb";
int mainstacksize = Stack;


static void
fsclunk(Fid* fid)
{
	int	i;
	Port*	p;
	File* f;

	if (fid->qid.type&QTAUTH){
		authdestroy(fid);
		return;
	}
	f = fid->file;
	if (f == nil)
		return;
	p = f->aux;
	if (p == nil)
		return;
	lock(&portlck);
	for (i = 0; i < Nreader; i++){
		if (p->rd[i].fid == fid){
			assert(!p->rd[i].r);
			free(p->rd[i].data);
			p->rd[i].data = nil;
			p->rd[i].fid = nil;
			break;
		}
	}
	if (fid->omode >= 0)
		p->nopens--;
	if (chatty9p)
		fprint(2, "orclose %d, nopens %d (fid %uld file %s)\n",
			p->orclose, p->nopens, fid->fid, f->name);
	if (p->orclose && p->nopens <= 0){
		/* We must incref the file because
		 * removefile assumes that we hold the
		 * reference given to it, and we do not.
		 * We just want the file removed from the tree.
		 *
		 * orclose must be checked because we want to
		 * remove files that have at least received a
		 * Tread. Otherwise, we would not be able to
		 * "touch /mnt/plumb/port" from the shell to
		 * create a port.
		 */
		incref(f);
		unlock(&portlck);
		removefile(f);
	} else
		unlock(&portlck);
		
}

static void
fsflush(Req* r)
{
	int	i;
	int	j;
	Port*	p;
	Req*	rr;

	lock(&portlck);
	for (i = 0; i < nports; i++){
		if (p = port[i]){
			for (j = 0; j < Nreader; j++){
				if (p->rd[j].r && p->rd[j].r->tag == r->ifcall.oldtag){
					rr = p->rd[j].r;
					p->rd[j].r = nil;
					unlock(&portlck);
					respond(rr, "flush");
					respond(r, nil);
					return;
				}
			}
		}
	}
	unlock(&portlck);
	fprint(2, "%s: no old req found\n", argv0);
	respond(r, nil);
}

static void
fscreate(Req* r)
{
	File*	file;
	Port*	p;
	char*	name;
	char*	uid;
	int	mode;
	File*	f;
	int	i;

	file = r->fid->file;
	name = r->ifcall.name;
	uid = r->fid->uid;
	mode = r->fid->file->mode & 0x777 & r->ifcall.perm;
	mode |= (r->ifcall.perm & ~0x777);
	if (mode&DMDIR){
		respond(r, "ports cannot be directories");
		return;
	}
	if(f = createfile(file, name, uid, mode, nil)){
		p = emalloc9p(sizeof *p);
		p->orclose = 0;
		p->rd[0].fid = r->fid;
		p->nopens = 1;
		f->aux = p;
		lock(&portlck);
		for (i = 0; i < nports; i++)
			if (port[i] == nil)
				break;
		assert(i < Nports);
		port[i] = p;
		if (i == nports)
			nports++;
		unlock(&portlck);
		closefile(r->fid->file);
		r->fid->file = f;
		r->ofcall.qid = f->qid;
		respond(r, nil);
	} else
		responderror(r);
}

static void
fsopen(Req* r)
{
	Port*	p;
	File*	file;

	if (r->fid->qid.type&QTAUTH){
		sysfatal("open for auth file");
		return;
	}
	file = r->fid->file;
	wlock(file);
	lock(&portlck);
	if (p = file->aux)
		p->nopens++;
	unlock(&portlck);
	wunlock(file);
	respond(r, nil);
}

static void
fsread(Req* r)
{
	Port*	p;
	int	i;
	void*	buf;
	File*	file;
	long	count;

	if (r->fid->qid.type&QTAUTH){
		authread(r);
		return;
	}
	if (r->fid->qid.type&QTDIR){
		respond(r, "bug: write on dir");
		return;
	}
	file = r->fid->file;
	count = r->ifcall.count;
	buf = r->ofcall.data;
	wlock(file);		// It's a reader, but we modify the port
	p = file->aux;
	p->orclose = 1;

	/* Try to service from a existing reader entry */
Read:
	for (i = 0; i < Nreader; i++){
		if (p->rd[i].fid != r->fid)
			continue;
		if (p->rd[i].data && p->rd[i].rdoff >= p->rd[i].ndata){
			free(p->rd[i].data);
			p->rd[i].rdoff = p->rd[i].ndata = 0;
			p->rd[i].data = nil;
		}
		if (p->rd[i].data){
			if (count > p->rd[i].ndata - p->rd[i].rdoff)
				count = p->rd[i].ndata - p->rd[i].rdoff;
			memmove(buf, p->rd[i].data + p->rd[i].rdoff, count);
			p->rd[i].rdoff += count;
			if (p->rd[i].rdoff >= p->rd[i].ndata){
				free(p->rd[i].data);
				p->rd[i].rdoff = p->rd[i].ndata = 0;
				p->rd[i].data = nil;
			}
			wunlock(file);
			r->ofcall.count = count;
			respond(r, nil);
			return;
		} else {
			assert(!p->rd[i].r);
			p->rd[i].r = r;
			wunlock(file);
			return;
		}
	}
	/* Allocate a reader if not yet allocated */
	for (i = 0; i < Nreader; i++){
		if (!p->rd[i].fid){
			p->rd[i].fid = r->fid;
			p->rd[i].r = nil;
			p->rd[i].data = nil;
			p->rd[i].ndata = p->rd[i].rdoff = 0;
			goto Read;
		}
	}
	wunlock(file);
	respond(r, "no more readers");
}

static void
sendmsg(Reader* rd, char* buf, long cnt)
{
	int	n;
	Req*	r;
	if (rd->data == nil){
		rd->data = emalloc9p(cnt);
		rd->ndata= cnt;
	} else {
		rd->data = erealloc9p(rd->data, rd->ndata + cnt);
		rd->ndata += cnt;
	}
	memmove(rd->data+rd->rdoff, buf, cnt);
	if (r = rd->r){
		rd->r = nil;
		n = r->ifcall.count;
		if (n > cnt)
			n = cnt;
		memmove(r->ofcall.data, rd->data+rd->rdoff, n);
		rd->rdoff += n;
		r->ofcall.count = n;
		respond(r, nil);
		if (rd->rdoff >= rd->ndata){
			free(rd->data);
			rd->data = nil;
			rd->ndata = rd->rdoff = 0;
		}
	}
}

static void
fswrite(Req* r)
{
	int	i;
	Port*	p;
	File*	file;
	long	count;
	vlong	offset;

	if (r->fid->qid.type&QTAUTH){
		authwrite(r);
		return;
	}
	if (r->fid->qid.type&QTDIR){
		respond(r, "bug: write on dir");
		return;
	}
	file = r->fid->file;
	offset = r->ifcall.offset;
	count = r->ifcall.count;
	r->ofcall.count = count;
	wlock(file);
	lock(&portlck);
	p = file->aux;
	for (i = 0; i < Nreader; i++){
		if (p->rd[i].fid){
			unlock(&portlck);
			sendmsg(p->rd+i, r->ifcall.data, count);
			lock(&portlck);
		}
	}
	unlock(&portlck);
	wunlock(file);
	respond(r, nil);
}

static void
fsfree(File *f)
{
	Port*	p;
	int	i;

	p = f->aux;
	f->aux = nil;
	if(p){
		for (i = 0; i < Nreader; i++)
			if (p->rd[i].data)
				free(p->rd[i].data);
		lock(&portlck);
		for (i = 0; i < nports; i++)
			if (port[i] == p){
				port[i] = nil;
				break;
			}
		assert(i < nports);
		while (i == nports - 1)
			nports--;
		unlock(&portlck);
		free(p);
	}
}

static void
fsattach(Req* r)
{
	if (r->srv->auth != nil && authattach(r) < 0)
		return;
	respond(r, nil);
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


static Srv sfs=
{
	.auth	=	bauth9p,
	.attach	=	fsattach,
	.open	=	fsopen,
	.create	=	fscreate,
	.read	=	fsread,
	.write	=	fswrite,
	.flush	= 	fsflush,
	.destroyfid = 	fsclunk,
};


static void
announceproc(void*)
{
	int	afd = -1;
	char*	cnstr;

	cnstr = strchr(vname, ' ');
	if (cnstr)
		*cnstr++ = 0;
	for(;;){
		afd = announcevol(afd, addr, vname, cnstr);
		sleep(10 * 1000);
	}
}

void
usage(void)
{
	fprint(2,"usage: %s [-abcdAD] [-s srv] [-m mnt] [-n addr] [-V vol]\n",argv0);
	exits("usage");
}

void
threadmain(int argc, char **argv)
{
	char*	mnt;
	char*	srv;
	int	mflag;

	srv = nil;
	mnt = nil;
	mflag = MREPL|MCREATE;
	ARGBEGIN{
	case 'a':
		mflag = MAFTER|MCREATE;
		break;
	case 'b':
		mflag = MBEFORE|MCREATE;
		break;
	case 'c':
		mflag = MREPL|MCREATE;
		break;
	case 'A':
		sfs.auth = nil;
		break;
	case 'D':
		chatty9p++;
		break;
	case 's':
		srv = EARGF(usage());
		break;
	case 'm':
		mnt = EARGF(usage());
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

	if (argc != 0)
		usage();
	if (getenv("local"))
		sfs.auth = nil;
	if (srv == nil && mnt == nil && addr == nil){
		mnt = "/devs/ports";
		addr = "tcp!*!11002";
	}
	if (!chatty9p)
		rfork(RFNOTEG);
	sfs.tree =  alloctree(nil, nil, DMDIR|0755, fsfree);
	if (addr != nil){
		threadlistensrv(&sfs, addr);
	}
	if (srv != nil || mnt != nil)
		threadpostmountsrv(&sfs, srv, mnt, mflag);
	if (addr != nil && vname != nil)
		proccreate(announceproc, 0, 8*1024);
	threadexits(nil);
}
