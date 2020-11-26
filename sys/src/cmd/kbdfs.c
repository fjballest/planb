#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <bio.h>
#include <thread.h>
#include <fcall.h>
#include <auth.h>
#include <draw.h>
#include <mouse.h>
#include <cursor.h>
#include <9p.h>
#include <b.h>

/* Pending:
 *	1. Needs a rewrite, along the lines of mousefs.c
 *	2. Should not send runes in the clear.
 */

typedef struct Dev	Dev;

enum {
	Qdir = 0,
	Qcons,
	Qctl,
	Qmax,

	Ndevs	= 64,
	Nevs	= 32,
	Stack	= 16 * 1024,

	Rreclaim	= L''	// rune to reclaim kbd (F1)
};

struct Dev {
	char	name;	// char to stampt events
	char*	addr;	// IP for the machine servicing this
	int	gone;
	int	con;	// to peer.
	Channel*evs;	// of char*; sent to peer.
};

int mainstacksize = Stack;

static char	Ebad[] = "something bad happened";
static char	Eperm[] =	"permission denied";
static char	Enotexist[] =	"file not found";
static char	Eattach[] = 	"unknown specifier in attach";
static char	Ebadctl[] =	"bad control request";
static Lock	devslock;
static Dev*	devs[Ndevs];
static int	ndevs;
static int	debug;
static Channel*	kbdc;	// devs[i].evs: where to send our events.
static char*	addr;
static char*	vname;

static char*	usr;
static char*	localaddr;
static int	doauth = 1;

#define dprint	if(debug)fprint
#define Dprint	if(debug>1)fprint


static int
alarmed(void *, char *s)
{
	if(strcmp(s, "alarm") == 0)
		return 1;
	return 0;
}

static char*
csquery(char *addr)
{
	static char buf[128];
	char* p;
	char* e;
	int fd, n;

	if (!addr || !strcmp(addr, "local"))
		return estrdup9p(localaddr);
	fd = open("/net/cs", ORDWR);
	if(fd < 0){
		fprint(2, "cannot open /net/cs: %r");
		return nil;
	}
	addr = netmkaddr(addr, "tcp", "11001");
	if(write(fd, addr, strlen(addr)) <= 0){
		fprint(2, "translating %s: %r\n", addr);
		close(fd);
		return nil;
	}
	seek(fd, 0, 0);
	buf[0] = 0;
	n = read(fd, buf, sizeof(buf)-1);
	close(fd);
	if (n < 0){
		fprint(2, "cannot read /net/cs: %r");
		return nil;
	}
	buf[n] = 0;
	p = strchr(buf, ' ');
	if (!p)
		return nil;
	p++;
	e = strchr(p, '!');
	if (e){
		// If it's the std service (port kbd), take just
		// the machine name.
		// For kbdfs's started at ports other than kbd,
		// take all the address, including the port.
		if (!strncmp(e+1, "11001", 5))
			*e = 0;
		else
			return p ? smprint("tcp!%s", p) : nil;
	}
	return p ? estrdup9p(p) : nil;
}

static int
islocal(char* addr)
{
	if (!addr || !strcmp(addr, "local"))
		return 1;
	if (!strcmp(addr, sysname()) || !strcmp(addr, localaddr))
		return 1;
	return 0;
}

static int
_setdest(char* addr)
{
	int	i;

	if (islocal(addr)){
		i = 0;
	} else {
		for (i = 1; i < ndevs; i++)
			if (devs[i]->addr != nil && !strcmp(addr, devs[i]->addr))
				break;
		if (i == ndevs)
			return 0;
	}
	kbdc = devs[i]->evs;	// send our events to it.
	dprint(2, "kbdfs: setdest %s\n", addr);
	return 1;
}

static int
setdest(char* addr)
{
	int	r;

	lock(&devslock);
	r = _setdest(addr);
	unlock(&devslock);
	return r;
}


static void
sendproc(void* a)
{
	Dev*	dev;
	Rune	r;
	char	s[UTFmax];

	dev = a;
	threadsetname("sendproc");
	while(recv(dev->evs, &r)>= 0 && r && !dev->gone){
		Dprint(2, "kbd sent: %C\n", r);
		runetochar(s, &r);
		if (write(dev->con, s, UTFmax) != UTFmax)
			break;
	}
	lock(&devslock);
	dprint(2, "kbdfs: %s sendproc exiting\n", dev->addr);
	free(dev->addr);
	dev->addr = nil;
	unlock(&devslock);
	r = 0;
	runetochar(s, &r);
	write(dev->con, s, UTFmax);
	close(dev->con);
	dev->con = -1;
	if (dev->evs == kbdc)
		setdest("local");
	threadexits(nil);
}

static void
hangupdev(Dev* d)
{
	Rune	z;

	if (d == devs[0])	// local never goes
		return;
	d->gone = 1;
	z = 0;
	send(d->evs, &z);
}

static Dev*
outgoing(void)
{
	int	i;

	lock(&devslock);
	for (i = 1; i < ndevs; i++)
		if (devs[i]->addr != nil && kbdc == devs[i]->evs){
			unlock(&devslock);
			return devs[i];
		}
	unlock(&devslock);
	return nil;
}

static int
gone(char* addr)
{
	int	i;

	/* We skip devs[0]. Local is never gone */

	lock(&devslock);
	for (i = 1; i < ndevs; i++)
		if (devs[i]->addr != nil && strstr(devs[i]->addr, addr)){
			if (kbdc == devs[i]->evs){
				dprint(2, "kbdfs: current gone\n");
				_setdest("local");
			}
			hangupdev(devs[i]);
			break;
		}
	unlock(&devslock);
	return i < ndevs;
}

static void
recvproc(void* a)
{
	char	buf[UTFmax];
	Rune	r;
	Dev*	d;
	int	fd;
	int	n;
	Dev*	outd;

	d = a;
	threadsetname("recvproc");
	fd = d->con;
	for(;;){
		n = read(d->con, buf, UTFmax);
		if (n < 0 || d->gone){
			dprint(2, "kbdfs: %s gone %d %d\n", d->addr, n, d->gone);
			break;
		}
		if (n == 0)
			continue;
		chartorune(&r, buf);
		if (!r){
			if (d == devs[0]){
				outd = outgoing();
				setdest("local");
				if (outd)
					hangupdev(outd);
				continue;
			} else {
				dprint(2, "kbdfs: %s end\n", d->addr);
				break;
			}
		}
		buf[n] = 0;
		Dprint(2, "kbd read: %C\n", r);
		if (d != devs[0]){
			if (kbdc == d->evs){
				// Got events from there.
				// cannot use d as the current output.
				gone(d->addr);
			}
		}
		if (kbdc != nil){
			buf[1] = d->name;
			send(kbdc, &r);
		} else
			fprint(2, "kbdfs: no kbdc\n");
	}
	dprint(2, "kbdfs: %s recvproc exiting\n", d->addr);
	if (d->evs == kbdc)
		setdest("local");
	r = 0;
	if (d != devs[0])
		send(d->evs, &r);
	d->con = -1;
	runetochar(buf, &r);
	write(fd, buf, UTFmax);
	close(fd);
	threadexits(nil);

}

static Dev*
newdev(char* addr, int fd)
{
	Dev*	dev;
	int	i;
	Rune	r;

	if (0 && strchr(addr, '!')){
		// that's ok with more than one kbdfs per machine.
		fprint(2, "kbdfs: newdev: bad address %s\n", addr);
		return nil;
	}
	if (islocal(addr))
		addr = localaddr;
	dprint(2, "kbdfs: newdev %s\n", addr);
	lock(&devslock);
	dev = nil;
	for (i = 0; i < ndevs; i++){
		if (devs[i]->addr == nil){
			dev = devs[i];
			while(nbrecv(devs[i]->evs, &r) > 0)
				;
			break;
		}
	}
	if (dev == nil){
		dev = emalloc9p(sizeof(Dev));
		dev->evs = chancreate(sizeof(Rune), Nevs);
		assert(ndevs < Ndevs);
		devs[ndevs++] = dev;
	}
	dev->gone = 0;
	dev->addr = estrdup9p(addr);
	dev->con = fd;
	unlock(&devslock);
	return dev;
}


static void
fsattach(Req *r)
{
	char *spec;

	spec = r->ifcall.aname;
	if(spec != nil && spec[0] != 0){
		respond(r, Eattach);
		return;
	}

	r->ofcall.qid = (Qid){Qdir, 0, QTDIR};
	r->fid->qid = r->ofcall.qid;
	respond(r, nil);
}

static void
fsopen(Req *r)
{
	int	p;

	r->ifcall.mode &= 3;
	if (r->fid->qid.type == QTAUTH || r->ifcall.mode == OREAD){
		r->fid->omode = r->ifcall.mode;
	} else {
		p = r->fid->qid.path;
		switch(p){
		case Qdir:
			respond(r, Eperm);
			return;
		case Qcons:
		case Qctl:
			r->fid->omode = r->ifcall.mode;
			break;
		default:
			respond(r, Ebad);
			return;
		}
	}
	respond(r, nil);
}

static int
dirgen(int i, Dir *d, void*)
{
	if(i > 1)
		return -1;
	memset(d, 0, sizeof *d);
	d->uid = estrdup9p("sys");
	d->gid = estrdup9p("sys");
	d->length = 0;
	switch(i){
	case -1:
		d->qid.type = QTDIR;
		d->qid.path = Qdir;
		d->mode = 0555|DMDIR;
		d->name = estrdup9p("/");
		break;
	case 0:
		d->mode = 0660;
		d->qid.path = Qcons;
		d->name = estrdup9p("cons");
		break;
	case 1:
		d->mode = 0220;
		d->qid.path = Qctl;
		d->name = estrdup9p("kbdctl");
		break;
	default:
		return -1;
	}
	return 0;
}

void
kreadproc(void* a)
{
	static char	locked = 0;
	Rune	ev;
	Req*	r;
	Channel* kreadreq = a;
	char	s[UTFmax+1];
	int	n;

	threadsetname("kreadproc");
	for(;;){
		r = recvp(kreadreq);

		if (recv(devs[0]->evs, &ev) < 0){
			fprint(2, "kbdfs: nil event?");
			break;
		}
		n = runetochar(s, &ev);
		r->ofcall.count = n;
		if (r->ifcall.count < r->ofcall.count)
			r->ofcall.count = r->ifcall.count;
		memmove(r->ofcall.data, s, r->ofcall.count);
		Dprint(2, "fsread: %s\n", s);
		respond(r, nil);
	}
}

static void
fsread(Req *r)
{
	static Channel* kreadreqc = nil;
	static char	locked = 0;
	char	m[200];
	char*	e;
	char*	c;
	int	p;
	int	i;


	p = r->fid->qid.path;
	switch(p){
	case Qdir:
		dirread9p(r, dirgen, nil);
		break;
	case Qcons:
		if (kreadreqc == nil){
			kreadreqc = chancreate(sizeof(Req*), Nevs);
			proccreate(kreadproc, kreadreqc, Stack);
		}
		sendp(kreadreqc, r);
		return;
	case Qctl:
		e = m;
		lock(&devslock);
		for (i = 0; i < ndevs; i++)
			if (devs[i]->addr){
				c =  (kbdc == devs[i]->evs) ? "*" : "";
				e = seprint(e, m+sizeof(m), "%s %s\n", devs[i]->addr, c);
			}
		unlock(&devslock);
		readstr(r, m);
		break;
	default:
		respond(r, Ebad);
		return;
	}
	respond(r, nil);
}

static int
authfd(int fd, char* role)
{
	AuthInfo*i;

	i = nil;
	alarm(5*1000);
	USED(i);
	i = auth_proxy(fd, nil, "proto=p9any user=%s role=%s", usr, role);
	alarm(0);
	if (i == nil)
		return 0;
	auth_freeAI(i);
	return 1;
}

static int
remote(char* addr)
{
	int	fd;
	Dev*	dev;

	if (islocal(addr)){
		dprint(2, "remote for local addr\n");
		return 1;
	}
	fd = dial(netmkaddr(addr, "tcp", "kbd"), 0, 0, 0);
	if (fd < 0){
		fprint(2, "kbdfs: %s: %r\n", addr);
		return 0;
	}
	if (doauth && !authfd(fd, "client")){
		fprint(2, "kbdfs: can't auth to %s: %r\n", addr);
		close(fd);
		return 0;
	}
	dev = newdev(addr, fd);
	assert(dev);
	proccreate(recvproc, dev, Stack);
	proccreate(sendproc, dev, Stack);
	return 1;
}

static char*
ctl(char* msg)
{
	char*	args[4];
	int	nargs;
	char*	addr;

	/* call address
	 * close address
	 */
	nargs = tokenize(msg, args, nelem(args));
	if (nargs != 2)
		return Ebadctl;
	addr = csquery(args[1]);
	if (strcmp(args[0], "call") == 0){
		if (!setdest(addr)){
			remote(addr);
			if (!setdest(addr)){
				free(addr);
				return Enotexist;
			}
		}
	} else if (strcmp(args[0], "close") == 0){
		if (!gone(addr)){
			free(addr);
			return Enotexist;
		}
	} else {
		free(addr);
		return Ebadctl;
	}
	free(addr);
	return nil;
}

static void
fswrite(Req *r)
{
	static int cfd = -1;
	int	p;
	char	msg[80];
	int	n;
	char*	e;

	p = r->fid->qid.path;
	switch(p){
	case Qcons:
		if (cfd < 0)
			cfd = open("#c/cons", OWRITE|OCEXEC);
		n = r->ifcall.count;
		r->ofcall.count = write(cfd, r->ifcall.data, n);
		break;
	case Qdir:
		respond(r, Ebad);
		return;
	case Qctl:
		n = r->ofcall.count = r->ifcall.count;
		if (n > sizeof(msg)-1)
			n = sizeof(msg)-1;
		memmove(msg, r->ifcall.data, n);
		msg[n] = 0;
		e = ctl(msg);
		if (e != nil){
			respond(r, e);
			return;
		}
		break;
	default:
		respond(r, Ebad);
		return;
	}
	respond(r, nil);
}

static char*
fswalk1(Fid *fid, char *name, Qid* q)
{

	if (fid->qid.path != Qdir){
		if(strcmp(name, "..") == 0){
			fid->qid.path = Qdir;
			fid->qid.type = QTDIR;
			*q = fid->qid;
			return nil;
		}
		return Enotexist;
	}
	if (strcmp(name, "..") == 0){
		fid->qid.path = Qdir;
		fid->qid.type = QTDIR;
	} else if (strcmp(name, "cons") == 0){
		fid->qid.path = Qcons;
		fid->qid.type = 0;
	} else if (strcmp(name, "kbdctl") == 0){
		fid->qid.path = Qctl;
		fid->qid.type = 0;
	} else
		return Enotexist;
	*q = fid->qid;
	return nil;
}

static void
fsstat(Req *r)
{
	int q;

	q = r->fid->qid.path;
	switch(q){
	case Qdir:
		dirgen(-1, &r->d, nil);
		break;
	case Qcons:
		dirgen(0, &r->d, nil);
		break;
	case Qctl:
		dirgen(1, &r->d, nil);
		break;
	}
	respond(r, nil);
}

static Srv mfs=
{
.attach=fsattach,
.open=	fsopen,
.read=	fsread,
.stat=	fsstat,
.write= fswrite,
.walk1= fswalk1,
};

static void
listener(void *a)
{
	int	afd, lfd;
	char	adir[40];
	char	ldir[40];
	char*	addr = a;
	int	dfd;
	NetConnInfo*i;
	Dev*	dev;

	threadsetname("listener");
	threadnotify(alarmed, 1);
	afd = announce(netmkaddr(addr, 0, "kbd"), adir);
	if (afd < 0)
		sysfatal("can't announce: %r");
	for(;;){
		lfd = listen(adir, ldir);
		if (lfd < 0)
			sysfatal("can't listen: %r");
		dfd = accept(lfd, ldir);
		i = getnetconninfo(ldir, dfd);
		close(lfd);
		if (doauth && !authfd(dfd, "server")){
			fprint(2, "auth failed for %s\n", i->rsys);
			close(dfd);
		} else {
			dev = newdev(i->rsys, dfd);
			dprint(2, "kbdfs: call from %s\n", dev->addr);
			setdest("local");
			proccreate(recvproc, dev, Stack);
			proccreate(sendproc, dev, Stack);
		}
		freenetconninfo(i);
	}
}

void
srvproc(void* a)
{
	int*	p = a;

	threadsetname("srvproc");
	threadnotify(alarmed, 1);
	close(p[1]);
	mfs.infd = p[0];
	mfs.outfd= p[0];
	mfs.nopipe= 1;
	mfs.srvfd = -1;
	srv(&mfs);
	fprint(2, "kbdfs: srvproc exiting\n");
	threadexits(nil);
}

void
setpri(int pri)
{
	int	pid;
	char*	s;
	int	fd;

	pid = getpid();
	s = smprint("/proc/%d/ctl", pid);
	fd = open(s, OWRITE);
	free(s);
	s = smprint("pri %d", pri);
	write(fd, s, strlen(s));
	free(s);
	close(fd);
}

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
	fprint(2, "usage: %s [-AdD] [-m mnt] [-a addr]\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char **argv)
{
	char*	mnt;
	int	p[2];
	int	kbdfd;

	mnt = "/dev";
	addr= "tcp!*!11001";
	ARGBEGIN{
	case 'A':
		doauth = 0;
		break;
	case 'D':
		chatty9p++;
	case 'd':
		debug++;
		break;
	case 'a':
	case 'n':
		addr = EARGF(usage());
		break;
	case 'm':
		mnt = EARGF(usage());
		break;
	case 'V':
		vname = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;

	if(argc!= 0)
		usage();

	chdir("/");
	localaddr=csquery(sysname());
	usr = getuser();
	if (usr == nil)
		usr = "none";
	setpri(13);
	procrfork(listener, estrdup9p(addr), Stack, RFNAMEG|RFFDG|RFNOTEG);

	kbdfd = 0; // open("/dev/cons", ORDWR|OCEXEC);
	if (kbdfd < 0)
		sysfatal("can't open cons: %r");
	newdev("local", kbdfd);
	setdest("local");
	procrfork(recvproc, devs[0], Stack, RFNAMEG|RFFDG|RFNOTEG);
	if(pipe(p) < 0)
		sysfatal("pipe: %r");
	procrfork(srvproc, p, Stack, RFNAMEG|RFFDG|RFNOTEG);
	close(p[0]);
	dprint(2, "kbdfs started\n");
	if (mount(p[1], -1, mnt, MBEFORE, "") < 0)
		sysfatal("mount: %r");
	dprint(2, "kbdfs mounted\n");
	if (addr != nil && vname != nil)
		proccreate(announceproc, 0, 8*1024);
	threadexits(nil);
}
