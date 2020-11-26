#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <auth.h>
#include <9p.h>
#include <b.h>

typedef struct Fproto Fproto;

enum {
	Stack	= 16 * 1024,

};

struct Fproto {
	char*	name;
	int	mode;
	int	id;
};


Req*	wreqs;
QLock	wreqslck;
int	waitingwreq;
int	afd = -1;
char*	addr;

enum {
	Qaudio,
	Qvolume,
};

Fproto	mp3dir[] = {
	{ "audio",  0222, Qaudio },
	{ "volume", 0666, Qvolume },
};

Fproto	prgdir[] = {
	{ "output",  0222, Qaudio },
};

#define dprint	if(debug)fprint
static int	debug;


static char*	playcmd = "mpeg3play -";
static char*	vname = "/devs/audio";
static int	mp3fd = -1;
static int	playerpid = -1;

static int	wflag;	// close the player after each write
static int	vflag;	// provide volume ctl file

static void
addwreq(Req* r)
{
	qlock(&wreqslck);
	if (waitingwreq){
		rendezvous(0, r);
		waitingwreq = 0;
	} else {
		r->aux = wreqs;
		wreqs = r;
	}
	qunlock(&wreqslck);
}

static Req*
getwreq(void)
{
	Req**	l;
	Req*	r;

	qlock(&wreqslck);
	for (l = &wreqs; *l != nil && (*l)->aux != nil; l = &((*l)->aux))
		;
	if (*l == nil){
		waitingwreq = 1;
		qunlock(&wreqslck);
		r = (Req*)rendezvous(0, 0);
		return r;
	} else {
		r = *l;
		*l = nil;
		qunlock(&wreqslck);
	}
	return r;
}

int
fileclass(File* fp)
{
	return ((fp->qid.path&0xff000000)>>24);
}

void
populate(File* d, Fproto* ent, int nent)
{
	int	i;
	File*	f;

	for (i = 0; i <nent; i++, ent++){
		f = createfile(d, ent->name, d->uid, ent->mode, nil);
		f->qid.path |= ((ent->id&0xff)<<24);
		closefile(f);
	}
}

void
mkfs(Srv* srv, Fproto* ent, int nent, int dmode)
{
	assert(srv->tree == nil);
	srv->tree =  alloctree(nil, nil, DMDIR|dmode, nil);
	populate(srv->tree->root, ent, nent);
}

void
stopplayer(int kill)
{
	dprint(2, "stopplayer\n");
	if (mp3fd >= 0)
		close(mp3fd);
	mp3fd = -1;
	if (kill){
		if (playerpid != -1)
			postnote(PNGROUP, playerpid, "kill");
		playerpid = -1;
	}
}


int
startplayer(void)
{
	int	fd[2];
	int	i;

	dprint(2, "startplayer\n");
	pipe(fd);
	switch(playerpid = rfork(RFFDG|RFREND|RFPROC|RFNOWAIT)){
	case -1:
		close(fd[0]);
		close(fd[1]);
		break;
	case 0:
		rfork(RFNOTEG);
		close(fd[1]);
		dup(fd[0], 0);
		close(fd[0]);
		for (i = 3; i < 50; i++)
			close(i);
		execl("/bin/rc", "rc", "-c", playcmd, nil);
		exits("exec");
		break;
	default:
		close(fd[0]);
		mp3fd = fd[1];
	}
	return mp3fd;
}

int
safewrite(int fd, Req* r)
{
	int	pid;
	Waitmsg*m;
	int	ok;

	r->ofcall.count = 0;
	switch(pid = rfork(RFPROC|RFMEM)){
	case 0:
		r->ofcall.count = write(fd, r->ifcall.data, r->ifcall.count);
		exits(0);
	case -1:
		sysfatal("safewrite");
	default:
		m = wait();
		if (m && m->pid == pid)
			ok = !m->msg[0];
		else
			ok = 0;
		free(m);
		return ok;
	}
}

static void
writeproc(void)
{
	Req*	r;

	for(;;){
		r = getwreq();
		dprint(2, "getwreq\n");
		assert(r);
		if (r->ifcall.offset == 0)
			stopplayer(1);
		if (mp3fd < 0)
			mp3fd = startplayer();
		if (mp3fd >= 0){
			if (!safewrite(mp3fd, r))
				stopplayer(0);
		}
		if (wflag)
			stopplayer(0);
		respond(r, nil);
	}
}

static void
fsread(Req* r)
{
	File* file;
	char*	buf;
	int	id;

	if (r->fid->qid.type&QTAUTH){
		authread(r);
		return;
	}
	file = r->fid->file;
	id = fileclass(file);
	switch(id){
	case Qaudio:
		respond(r, "bug: read of audio");
		break;
	case Qvolume:
		buf = readfstr("/dev/volume");
		readstr(r, buf ? buf : "");
		free(buf);
		break;
	default:
		respond(r, "bug: fsread: bad id");
	}

}

static void
fswrite(Req* r)
{
	File* file;
	int	id;

	if (r->fid->qid.type&QTAUTH){
		authwrite(r);
		return;
	}
	file = r->fid->file;
	id = fileclass(file);
	switch(id){
	case Qaudio:
		addwreq(r);
		break;
	case Qvolume:
		assert(vflag);
		writef("/dev/volume", r->ifcall.data, r->ifcall.count);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		break;
	default:
		respond(r, "bug: bad id in fspread");
	}
}

static void
fsattach(Req* r)
{
	if (r->srv->auth != nil && authattach(r) < 0)
		return;
	respond(r, nil);
}

static void
fsopen(Req* r)
{

	if (r->fid->qid.type&QTAUTH)
		sysfatal("fsopen for an AUTH file. fixme: add respond/return\n");
	respond(r, nil);
}

static void
fsclunk(Fid* fid)
{

	if (fid->qid.type&QTAUTH)
		authdestroy(fid);
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
	.read	=	fsread,
	.write	=	fswrite,
	.destroyfid = 	fsclunk,
};

static void
announceproc(void)
{
	int	afd = -1;

	for(;;){
		afd = announcevol(afd, addr, vname, nil);
		sleep(10 * 1000);
	}
}

void
usage(void)
{
	fprint(2, "usage: %s [-abcdADwv] [-V name] [-s srv] [-m mnt] [-n addr] [-p playcmd]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
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
	case 'd':
		debug++;
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
	case 'p':
		playcmd = EARGF(usage());
		break;
	case 'w':
		wflag = 1;
		break;
	case 'v':
		vflag = 1;
		break;
	case 'V':
		vname = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;

	if (argc != 0)
		usage();

	if (srv == nil && mnt == nil && addr == nil)
		addr=smprint("tcp!%s!11004", sysname());

	if (vflag)
		mkfs(&sfs, mp3dir, nelem(mp3dir), 0555);
	else
		mkfs(&sfs, prgdir, nelem(prgdir), 0555);
	if (rfork(RFPROC|RFMEM|RFNOWAIT) == 0)
		writeproc();
	if (addr != nil)
		listensrv(&sfs, addr);
	if (srv != nil || mnt != nil)
		postmountsrv(&sfs, srv, mnt, mflag);
	if (addr != nil && vname != nil){
		if (rfork(RFPROC|RFMEM|RFNOWAIT) == 0)
			announceproc();
	}
	exits(0);
}
