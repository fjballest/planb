#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <bio.h>
#include <thread.h>
#include <fcall.h>
#include <auth.h>
#include <9p.h>
#include <b.h>
#include "x10.h"


static X10* x;
static char myhc;

/*
 * 0 -> cm11
 * 1..n -> devices
 */

enum {
	Qdir = 0,
	Qcm11= 256,
	// 1..n -> devices

	Qauth= 128,
	// 128..m -> auth files

	Nauth = 2,
};

static char	Eperm[] =	"permission denied";
static char	Egone[] = 	"device is gone";
static char	Enotexist[] =	"file does not exist";
static char	Ebadctl[] =	"unknown control message";
static char	Eattach[] = 	"invalid attach specifier";
static char	Ex10[]=		"X10 request failed";
static char	Eauth[]=	"authentication failed";

static char*	uids[Ndevs+1];
static char*	gids[Ndevs+1];
static int	modes[Ndevs+1];
static char*	names[Ndevs+1];
static int	vers;

static AuthRpc*	authf[Nauth];
static int	authok[Nauth];
static char*	authu[Nauth];

/*
 * simplistic permission checking.  assume that
 * each user is the leader of her own group.
 */
static int
allowed(int qid, char *uid, int p)
{
	static char* elf;
	int m;
	int mode;
	char* g;
	char* u;

	if (elf == nil){
		elf=getenv("user");
		u = strchr(elf, '\n');
		if (u != nil)
			*u = 0;
	}
	switch(qid){
	case Qdir:
		mode = 0775;
		break;
	case Qcm11:
		qid = 0;
		// and fall...
	default:
		if (modes[qid])
			mode = modes[qid];
		else
			mode = 0664;
	}
	if (qid >= 0 && gids[qid])
		g = gids[qid];
	else
		g = "sys";
	if (qid >= 0 && uids[qid])
		u = uids[qid];
	else
		u = "sys";

	m = mode & 7;	/* other */
	if((p & m) == p)
		return 1;

	if(strcmp(u, uid) == 0 || strcmp(uid, elf) == 0) {
		m |= (mode>>6) & 7;
		if((p & m) == p)
			return 1;
	}

	if(strcmp(g, uid) == 0) {
		m |= (mode>>3) & 7;
		if((p & m) == p)
			return 1;
	}

	return 0;
}

static int
dirgen(int i, Dir *d, void*)
{
	int	n;
	Dev*	devs;
	char	name[40];
	memset(d, 0, sizeof *d);
	d->uid = estrdup9p("sys");
	d->gid = estrdup9p("sys");
	d->length = 0;
	d->atime = d->mtime = time(nil);
	switch(i){
	case -1:
		d->name = estrdup9p("/");
		d->qid.type = QTDIR;
		d->qid.path = Qdir;
		d->mode = DMDIR|0775;
		break;
	case 0:
		d->name = estrdup9p("cm11");
		d->qid.type = 0;
		d->qid.path = Qcm11;
		d->qid.vers = vers;
		d->mode = 0664;
		if (uids[0] != nil){
			free(d->uid);
			d->uid = estrdup9p(uids[0]);
		}
		if (gids[0] != nil){
			free(d->gid);
			d->gid = estrdup9p(gids[0]);
		}
		if (modes[0] != 0)
			d->mode = modes[0]&0777;
		break;
	default:
		devs = x10devs(x);
		for (n = 0; n < Ndevs; n++)
			if (devs[n].hc !=0 && --i == 0)
				break;
		if (n == Ndevs)
			return -1;
		if (names[n+1] == nil){
			name[0] = hctochr(devs[n].hc);
			seprint(name+1, name+sizeof(name), "%d",
				dctoint(devs[n].dc));
			d->name = estrdup9p(name);
		} else
			d->name = estrdup9p(names[n+1]);
		d->qid.type = 0;
		d->qid.path = n+1;
		d->qid.vers = vers;
		d->mode = 0664;
		if (uids[n+1] != nil){
			free(d->uid);
			d->uid = estrdup9p(uids[n+1]);
		}
		if (gids[n+1] != nil){
			free(d->gid);
			d->gid = estrdup9p(gids[n+1]);
		}
		if (modes[n+1] != 0)
			d->mode = modes[n+1]&0777;
	}
	return 0;
}

static char *keyspec = "proto=p9any role=server";

static void
fsauth(Req* r)
{
	int	i;
	int	afd;
	int	fd;

	for (i = 0; i < Nauth; i++)
		if (authf[i] == nil)
			break;
	if (i == Nauth){
		respond(r, "No more auth files");
		return;
	}
	r->afid->qid.path = Qauth+i;
	r->afid->qid.type = QTAUTH;
	r->afid->omode = ORDWR;
	r->ofcall.qid = r->afid->qid;

	if(access("/mnt/factotum", 0) < 0)
		if((fd = open("/srv/factotum", ORDWR)) >= 0)
			mount(fd, -1, "/mnt", MBEFORE, "");
	afd = open("/mnt/factotum/rpc", ORDWR);
	if (afd < 0)
		goto fail;
	authf[i] = auth_allocrpc(afd);
	authok[i] = 0;
	if (authf[i] == nil)
		goto fail;
	if(auth_rpc(authf[i], "start", keyspec, strlen(keyspec)) != ARok)
		goto fail;
	if (r->ifcall.uname == nil || r->ifcall.uname[0] == 0)
		goto fail;
	authu[i] = estrdup9p(r->ifcall.uname);
	respond(r, nil);
	return;
fail:
	if (afd >= 0)
		close(afd);
	free(authf[i]);
	authf[i] = nil;
	respond(r, Eauth);
}

static void
clrauth(int i)
{
	if (authf[i] == nil)
		return;
	free(authu[i]);
	close(authf[i]->afd);
		free(authf[i]);
	authu[i] = nil;
	authf[i] = nil;
	authok[i] = 0;
}

static long
_authread(Fid* fid, void* data, long count)
{
	int	i;
	long	n;
	int	rr;
	AuthInfo*ai;

	if (fid->qid.type != QTAUTH)
		return -1;
	i = fid->qid.path - Qauth;
	if (i < 0 ||  i >= Nauth || authf[i] == nil)
		return -1;
	rr = auth_rpc(authf[i], "read", nil, 0);
	n = 0;
	switch(rr){
	case ARdone:
		ai = auth_getinfo(authf[i]);
		if(ai == nil){
			clrauth(i);
			return -1;
		}
		auth_freeAI(ai);
		if (debug)
			fprint(2, "user %s authenticated\n", fid->uid);
		authok[i] = 1;
		break;
	case ARok:
		if(count < authf[i]->narg){
			clrauth(i);
			return -1;
		}
		memmove(data, authf[i]->arg, authf[i]->narg);
		n = authf[i]->narg;
		break;
	case ARphase:
	default:
		clrauth(i);
		return -1;
	}
	return n;
}

static void
fsauthread(Req* r)
{
	int	n;

	n = _authread(r->fid, r->ofcall.data, r->ifcall.count);
	if (n < 0)
		respond(r, Eauth);
	r->ofcall.count = n;
	respond(r, nil);
}

static void
fsauthwrite(Req* r)
{
	int ret;
	int	i;

	i = r->fid->qid.path - Qauth;
	if (i < 0 ||  i >= Nauth || authf[i] == nil){
		respond(r, Enotexist);
		return;
	}
	ret = auth_rpc(authf[i], "write", r->ifcall.data, r->ifcall.count);
	if(ret != ARok){
		clrauth(i);
		respond(r, Eauth);
		return;
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static void
fsattach(Req *r)
{
	char *spec;
	int	i;
	uchar	buf[1];

	if (r->afid == nil){
		respond(r, Eauth);
		return;
	}
	i = r->afid->qid.path - Qauth;
	if (i < 0 || i >= Nauth || authf[i] == nil){
		respond(r, Eauth);
		return;
	}
	if (!authok[i] && _authread(r->afid, buf, 0) != 0){
		clrauth(i);
		respond(r, Eauth);
		return;
	}
	authok[i] = 1;
	if (strcmp(authu[i], r->fid->uid) != 0){
		clrauth(i);
		respond(r, Eauth);
		return;
	}
	clrauth(i);
	x10reqsts(x);
	vers++;
	spec = r->ifcall.aname;
	if(spec != nil && spec[0] != 0){
		respond(r, Eattach);
		return;
	}

	r->ofcall.qid = (Qid){0, 0, QTDIR};
	r->fid->qid = r->ofcall.qid;
	respond(r, nil);
}

static void
fsopen(Req *r)
{
	ulong qid;
	int mode;

	r->ifcall.mode &= 3;
	if (r->fid->qid.type == QTAUTH){
		r->fid->omode = r->ifcall.mode;
		respond(r, nil);
		return;
	}
	switch(r->ifcall.mode){
	case OREAD:
		mode = AREAD;
		break;
	case OWRITE:
		mode = AWRITE;
		break;
	case ORDWR:
		mode = AREAD|AWRITE;
		break;
	default:
		respond(r, Eperm);
		return;
	}
	qid = r->fid->qid.path;
	if (qid >= 0 && qid <= Ndevs){
		r->fid->qid.vers = vers;
		r->ofcall.qid = r->fid->qid;
	}
	if (!allowed(qid, r->fid->uid, mode)	||
	    (r->ifcall.mode != OREAD && qid == Qdir)	){
		respond(r, Eperm);
		return;
	}
	r->fid->omode = r->ifcall.mode;
	respond(r, nil);
}

static void
fsread(Req *r)
{
	static char	null[] = "";
	static char*	ons[] = {"off", "on"};
	Dev*	devs;
	ulong	qid;
	int	o;
	char	buf[80];
	static	ulong	laststs;
	ulong	t;

	qid = r->fid->qid.path;
	if (r->fid->qid.type == QTAUTH){
		if (debug)
			fprint(2, "auth read\n");
		fsauthread(r);
		return;
	}
	o = r->fid->omode & 3;
	if (o != OREAD && o != ORDWR){
		respond(r, Eperm);
		return;
	}
	switch(qid){
	case Qdir:
		x10reqsts(x);
		vers++;
		dirread9p(r, dirgen, nil);
		break;
	case Qcm11:
		cm11sprint(x, buf, sizeof(buf));
		readstr(r, buf);
		break;
	default:
		if (!laststs)
			laststs = time(nil);
		t = time(nil);
		if (t - laststs >= 10){
			if (x10reqsts(x) < 0){
				sleep(500);
				if (x10reqsts(x) < 0){
					respond(r, Ex10);
					return;
				}
			}
			laststs = time(nil);
		}
		devs = x10devs(x);
		if (qid <= 0 || qid > Ndevs || devs[qid-1].hc ==0){
			respond(r, Egone);
			return;
		}
		readstr(r, ons[devs[qid-1].on]);
	}
	respond(r, nil);
}

/*
 * Process command as-is
 */
static char*
cm11cmd(char* cmd)
{
	char*	args[10];
	int	nargs;
	int	r;
	char	saved[40];
	int	attempts;

	if (debug)
		fprint(2, "cm11cmd %s ", cmd);
	strecpy(saved, saved+sizeof(saved), cmd);
	nargs = tokenize(cmd, args, nelem(args));
	if (nargs == 0){
		if (debug)
			fprint(2, "ignored\n");
		return nil;
	}
	if (nargs == nelem(args))
		sysfatal("bug: sh: fixme: Nargs overflow");
	if (*args[1] != myhc){
		if (debug)
			fprint(2, "ignored\n");
		return nil;
	}
	if (debug)
		fprint(2, "\n");
	args[nargs] = nil;
	attempts = 0;
again:
	r = runfunc(x, nargs, args);

	if (r == 0){
		syslog(0, logf, "cm11cmd %s: bad ctl", saved);
		return Ebadctl;
	}
	if (r <  0){
		if (attempts++ < 10){
			sleep(1000);
			goto again;
		}
		syslog(0, logf, "cm11cmd %s failed", saved);
		return Ex10;
	}
	syslog(0, logf, "cm11cmd %s", saved);
	return nil;
}

/*
 * process "first word" + house + device + "rest"
 */
static char*
devcmd(char* cmd, uchar hc, uchar dc)
{
	char	ncmd[50];
	char*	s;

	s = strchr(cmd, ' ');
	if (s == nil)
		s = "";
	else
		*s++= 0;
	seprint(ncmd, ncmd+sizeof(ncmd), "%s %c %d %s", cmd, hctochr(hc), dctoint(dc), s);
	return cm11cmd(ncmd);
}

static void
fswrite(Req *r)
{
	static char	buf[30];
	Dev*	devs;
	ulong	qid;
	char*	e;
	int	l;
	ulong	o;

	if (r->fid->qid.type == QTAUTH){
		fsauthwrite(r);
		return;
	}
	o = r->fid->omode & 3;
	if (o != OWRITE && o != ORDWR){
		respond(r, Eperm);
		return;
	}
	r->ofcall.count = r->ifcall.count;
	if (r->ofcall.count > sizeof(buf)-1)
		r->ofcall.count = sizeof(buf)-1;
	memmove(buf, r->ifcall.data, r->ofcall.count);
	buf[r->ofcall.count] = 0;
	l = strlen(buf);
	if (l > 0 && buf[l-1] == '\n')
		buf[l-1] = 0;
	qid = r->fid->qid.path;
	switch(qid){
	case Qdir:
		respond(r, Eperm);
		return;
	case Qcm11:
		e = cm11cmd(buf);
		if (e != nil){
			respond(r, e);
			return;
		}
		break;
	default:
		devs = x10devs(x);
		if (qid <= 0 || qid > Ndevs || devs[qid-1].hc ==0){
			respond(r, Egone);
			return;
		}
		// BUG: should handle 'on <name>' besides 'on <hc> <dc>'
		e = devcmd(buf, devs[qid-1].hc, devs[qid-1].dc);
		if (e != nil){
			respond(r, e);
			return;
		}
		break;
	}
	respond(r, nil);
	// and update our status now that it changed
	x10reqsts(x);
	vers++;
}

static void
fsstat(Req *r)
{
	ulong qid;

	qid = r->fid->qid.path;
	dirgen(qid == Qdir ? -1 : qid, &r->d, nil);
	respond(r, nil);
}

static char*
fswalk1(Fid *fid, char *name, void*)
{
	int	n;
	uchar	hc;
	uchar	dc;
	Dev*	devs;
	int	i;

	if(strcmp(name, "..") == 0) // we're in /
		return nil;
	if (strcmp(name, "cm11") == 0){
		fid->qid.type = 0;
		fid->qid.path = Qcm11;
		return nil;
	}
	for (i = 0; i < Ndevs; i++)
		if (names[i] != nil && strcmp(names[i], name) == 0){
			fid->qid.type = 0;
			fid->qid.path = i;
			return nil;
		}
	if (name[0] >= 'a' && name[0] <= 'p' ){
		hc = chrtohc(name[0]);
		n = atoi(name+1);
		dc = inttodc(n);
		devs = x10devs(x);
		for (i = 0; i < Ndevs; i++)
			if (devs[i].hc == hc && devs[i].dc == dc){
				fid->qid.type = 0;
				fid->qid.path = i+1;
				return nil;
			}
	}
	return Enotexist;
}

static char*
fsclone(Fid *, Fid*, void*)
{
	return nil;
}

static void
fswalk(Req *r)
{
	//x10reqsts(x);
	walkandclone(r, fswalk1, fsclone, nil);
}

static void
fswstat(Req* r)
{
	ulong	qid;
	Dev*	devs;

	qid = r->fid->qid.path;
	switch(qid){
	case Qdir:
		respond(r, Eperm);
		return;
	case Qcm11:
		qid = 0;
		goto chperm;
	default:
		devs = x10devs(x);
		if (qid <= 0 || qid > Ndevs || devs[qid-1].hc ==0){
			respond(r, Egone);
			return;
		}
	chperm:
		if (r->d.uid && r->d.uid[0]){
			if (uids[qid] != nil && strcmp(uids[qid], r->d.uid) != 0 ){
				respond(r, Eperm);
				return;
			}
			uids[qid] = strdup(r->d.uid);
		}
		if (r->d.gid && r->d.gid[0]){
			if (gids[qid] != nil){
				respond(r, Eperm);
				return;
			}
			gids[qid] = strdup(r->d.gid);
		}
		if (~(ulong)r->d.mode){
			if (!uids[qid] && modes[qid] != 0){
				respond(r, Eperm);
				return;
			}
			if (uids[qid] && strcmp(r->fid->uid, uids[qid])){
				respond(r, Eperm);
				return;
			}
			modes[qid] = r->d.mode;
		}
	}
	respond(r, nil);
}

static Channel*	reqc;
static Channel*	waitc;

static void
fssend(Req* r)
{
	sendp(reqc, r);
	recvp(waitc);
}

static void
shutdown(Srv* p)
{
	close(p->infd);
	free(p);
	threadexits("done");
}

static void
fsproc(void*)
{
	Req*	r;

	threadsetname("fsproc");
	for(;;){
		r = recvp(reqc);
		switch(r->ifcall.type){
		case Tauth:
			fsauth(r);
			break;
		case Tattach:
			fsattach(r);
			break;
		case Topen:
			fsopen(r);
			break;
		case Tread:
			fsread(r);
			break;
		case Twrite:
			fswrite(r);
			break;
		case Tstat:
			fsstat(r);
			break;
		case Twstat:
			fswstat(r);
			break;
		case Twalk:
			fswalk(r);
			break;
		default:
			respond(r, "bug in fsproc: bad ifcall type");
			break;
		}
		sendp(waitc, nil);
	}
}

static Srv sfs=
{
.auth=  fssend,
.attach=fssend,
.open=	fssend,
.read=	fssend,
.write= fssend,
.stat=	fssend,
.wstat=	fssend,
.walk=	fssend,
.end=	shutdown,
};

static void
perm(char* ln)
{
	char*	args[10];
	int	nargs;
	int	n;
	// perm name owner group mode
	nargs = tokenize(ln, args, nelem(args));
	if (nargs != 5)
		return;
	if (*args[1] != myhc)
		return;
	n = atoi(args[1]+1);
	if (n <0 || n >= Ndevs)
		return;
	uids[n] = strdup(args[2]);
	gids[n] = strdup(args[3]);
	modes[n]= strtol(args[4], nil, 0);
}

static void
setname(char* ln)
{
	char*	args[10];
	int	nargs;
	int	n;
	// name a1 str
	nargs = tokenize(ln, args, nelem(args));
	if (nargs != 3)
		return;
	if (*args[1] != myhc)
		return;
	n = atoi(args[1]+1);
	if (n <0 || n >= Ndevs)
		return;
	names[n] = estrdup9p(args[2]);
}

static void
config(char* conf)
{
	Biobuf*	bc;
	char*	ln;
	char*	c;
	if (conf == nil)
		return;
	bc = Bopen(conf, OREAD);
	if (bc == nil)
		return;
	while(ln = Brdstr(bc, '\n', 1)){
		c = strchr(ln, '#');
		if (c != nil)
			*c = 0;
		if (ln[0] == '\n' || ln[0] == '#' || ln[0] == 0)
			continue;
		if (strncmp("perm", ln, 4) == 0)
			perm(ln);
		else if (strncmp("name", ln, 4) == 0)
			setname(ln);
		else
			cm11cmd(ln);
	}
	Bterm(bc);
}



typedef struct PArg PArg;
struct PArg {
	int lfd;
	char ldir[40];
};

static void
srvproc(void* a)
{
	PArg*	p;
	Srv*	msrv;
	int	dfd;

	p = a;
	threadsetname("srvproc");
	dfd = accept(p->lfd, p->ldir);
	if (dfd < 0){
		free(p);
		threadexits(nil);
	}
	close(p->lfd);
	free(p);

	msrv = emalloc9p(sizeof(Srv));
	*msrv = sfs;
	msrv->infd = dfd;
	msrv->outfd= dfd;
	msrv->srvfd= -1;
	msrv->nopipe= 1;

	rfork(RFNOTEG);
	srv(msrv);

	threadexits(nil);
}

static void
listener(void *)
{
	int	afd, lfd;
	char	adir[40];
	char	ldir[40];
	PArg*	p;

	threadsetname("listener");
	afd = announce("tcp!*!19000", adir);
	if (afd < 0)
		sysfatal("can't announce: %r");
	for(;;){
		lfd = listen(adir, ldir);
		if (lfd < 0)
			sysfatal("can't listen: %r");
		p = emalloc9p(sizeof(PArg));
		p->lfd = lfd;
		strcpy(p->ldir, ldir);
		proccreate(srvproc, p, 32*1024);
	}
}

static void
announceproc(void*)
{
	int	afd = -1;
	char*	addr;

	addr = smprint("tcp!%s!19000 ", sysname());
	
	for(;;){
		afd = announcevol(afd, addr, "/devs/x10", nil);
		sleep(10 * 1000);
	}
}

void
pfs(X10* p, char hc, char* conf)
{
	x = p;
	myhc = hc;

	config(conf);
	x10reqsts(x);
	reqc = chancreate(sizeof(void*), 0);
	waitc = chancreate(sizeof(void*), 0);
	if (proccreate(announceproc, nil, 8*1024) < 0)
		sysfatal("procrfork: %r");
	if (proccreate(listener, nil, 32*1024) < 0)
		sysfatal("procrfork: %r");
	proccreate(fsproc, nil, 32*1024);
	syslog(0, logf, "X10 house %c started", hc);
}
