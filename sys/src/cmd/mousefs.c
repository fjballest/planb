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

typedef struct Dev	Dev;

enum {
	Qdir = 0,
	Qmouse,
	Qctl,
	Qmax,

	Ndevs	= 64,
	Nevs	= 128,
	Stack	= 16 * 1024,

	Mevlen	= 1+12*4,	// see mouse(3).

	Stdxmax	= 1280,
	Stdymax	= 1024,


};

struct Dev {
	char	name;		// char to stampt events
	char	addr[50];	// IP for the machine servicing this
	int	con;		// to/from peer.
	int	cfd;		// ctl fd for con (for hanging up)
	Channel*evs;		// of char*; got from peer
	Channel*out;		// of char*; sent to peer
	int	sender;		// true if senderproc alive
	int	receiver;	// true if receiverproc alive
};

static int	newpeer(char* addr);

int mainstacksize = Stack;

static char	Ebad[] = "something bad happened";
static char	Eperm[] =	"permission denied";
static char	Enotexist[] =	"file not found";
static char	Eattach[] = 	"unknown specifier in attach";
static char	Ebadctl[] =	"bad control request";
static Lock	devslock;
static Dev	devs[2];
static int	debug;

static int	outm;	// output device
static int	inm;	// input device

static char*	kbdctl = "/dev/kbdctl";
static char*	localaddr;
static int	doauth = 1;
static char*	vname;
static char*	addr;
static Ref	readers;
static int	mousefd = -1;

#define dprint	if(debug)fprint
#define Dprint	if(debug>1)fprint

static int	xmin, xmax, ymin, ymax;

static Cursor whitearrow = {
	{0, 0},
	{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0xFF, 0xFC, 
	 0xFF, 0xF0, 0xFF, 0xF0, 0xFF, 0xF8, 0xFF, 0xFC, 
	 0xFF, 0xFE, 0xFF, 0xFF, 0xFF, 0xFE, 0xFF, 0xFC, 
	 0xF3, 0xF8, 0xF1, 0xF0, 0xE0, 0xE0, 0xC0, 0x40, },
	{0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0x06, 0xC0, 0x1C, 
	 0xC0, 0x30, 0xC0, 0x30, 0xC0, 0x38, 0xC0, 0x1C, 
	 0xC0, 0x0E, 0xC0, 0x07, 0xCE, 0x0E, 0xDF, 0x1C, 
	 0xD3, 0xB8, 0xF1, 0xF0, 0xE0, 0xE0, 0xC0, 0x40, }
};


static void
changecursor(int w)
{
	static char curs[2*4+2*2*16];
	static int cfd = -1;
	static int dummy;
	static int last = -1;

	if (w == last)
		return;
	if (cfd < 0){
		Cursor* c = &whitearrow;
		cfd = open("/dev/cursor", OWRITE);
		BPLONG(curs+0*4, c->offset.x);
		BPLONG(curs+1*4, c->offset.y);
		memmove(curs+2*4, c->clr, 2*2*16);
	}
	if (cfd >=0){
		if (w)	// restore to default.
			write(cfd, curs, sizeof curs);
		else
			write(cfd, &dummy, sizeof(dummy));
	}
	last = w;
}

static int
alarmed(void *, char *s)
{
	if(strcmp(s, "alarm") == 0)
		return 1;
	return 0;
}

static void
getsizes(void)
{
	int	fd;
	char	buf[80];
	int	n;
	char*	p;
	char*	e;

	fd = open("/dev/wctl", OREAD);
	if (fd < 0){
		p = getenv("vgasize");
		if (p == nil)
			p = strdup("1024x768x8");
		strcpy(buf, p);
		free(p);
		p = strchr(buf, 'x');
		*p++ = 0;
		xmax = strtol(buf, 0, 0);
		e = strchr(p, 'x');
		*e = 0;
		ymax = strtol(p, 0, 0);
		xmin = ymin = 0;
	} else {
		n = read(fd, buf, sizeof(buf)-1);
		close(fd);
		buf[n] = 0;
		p = buf;
		xmin = strtol(p, &p, 0);
		ymin = strtol(p, &p, 0);
		xmax = strtol(p, &p, 0);
		ymax = strtol(p, &p, 0);
	}
	Dprint(2, "wsize: %d %d %d %d\n", xmin, ymin, xmax, ymax);
}

static int
isreclaim(char* ev)
{
	char	buf[60];
	int	x, y, b;
	char*	p;

	strecpy(buf, buf+sizeof(buf), ev);
	x = strtol(buf+2, &p, 0);
	y = strtol(p, &p, 0);
	b = strtol(p, &p, 0);
	return ( x - xmin < 10 && y - ymin < 10 && b == 1);
}

static int
htonmouse(char* ev)
{
	char	buf[60];
	int	x, y, b;
	ulong	m;
	double	xscale, yscale;
	char*	p;
	int	isnop;

	strecpy(buf, buf+sizeof(buf), ev);
	x = strtol(buf+2, &p, 0);
	y = strtol(p, &p, 0);
	b = strtol(p, &p, 0);
	m = strtoul(p, &p, 0);
	isnop = (x == 0 && y == 0 && b == 0);
	x -= xmin;
	y -= ymin;
	xscale = (double)Stdxmax / (double)(xmax - xmin);
	yscale = (double)Stdymax / (double)(ymax - ymin);
	x = (int)(xscale * (double)x);
	y = (int)(yscale * (double)y);
	seprint(buf+2, buf+sizeof(buf), "%10d %11d %11d %11lud ", x, y, b, m);
	strcpy(ev, buf);
	return !isnop;
}

static void
ntohmouse(char* ev)
{
	char	buf[60];
	int	x, y, b;
	ulong	m;
	double	xscale, yscale;
	char*	p;

	Dprint(2, "n m: %s\n", ev);
	strecpy(buf, buf+sizeof(buf), ev);
	x = strtol(buf+2, &p, 0);
	y = strtol(p, &p, 0);
	b = strtol(p, &p, 0);
	m = strtoul(p, &p, 0);
	xscale = (double)(xmax - xmin) / Stdxmax;
	yscale = (double)(ymax - ymin) / Stdymax;
	x = (int)(xscale * (double)x);
	y = (int)(yscale * (double)y);
	x += xmin;
	y += ymin;
	seprint(buf+2, buf+sizeof(buf), "%10d %11d %11d %11lud ", x, y, b, m);
	strcpy(ev, buf);
	Dprint(2, "h m: %s\n", ev);
}

static void
fstohmouse(char* ev)
{
	char	buf[60];
	int	x, y;
	char*	p;

	strecpy(buf, buf+sizeof(buf), ev);
	x = strtol(buf+1, &p, 0);
	y = strtol(p, &p, 0);
	seprint(buf, buf+sizeof(buf), "m%11d %11d %11d %11lud ", x, y, 0, 0UL);
	strcpy(ev, buf);
}

static char*
csquery(char *addr)
{
	static char buf[128];
	char* p;
	int fd, n;

	if (localaddr != nil)
	if (!addr || !strcmp(addr, "local"))
		return estrdup9p(localaddr);
	fd = open("/net/cs", ORDWR);
	if(fd < 0){
		fprint(2, "cannot open /net/cs: %r");
		return nil;
	}
	addr = netmkaddr(addr, "tcp", "11000");
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
	if (strchr(p, '!'))
		return smprint("tcp!%s", p);
	else {
		addr = smprint("tcp!%s!%s", sysname(), p);
		return csquery(addr);
	}
}

static int
islocal(char* a)
{
	char*	la;

	if (!a || !strcmp(a, "local"))
		return 1;
	if (!strcmp(a, sysname()) || !strcmp(a, localaddr))
		return 1;

	la = csquery(addr);
	if (la){
		if (!strcmp(a, la)){
			free(la);
			return 1;
		}
		free(la);
	}

	return 0;
}

static void
kbdsetdest(char* addr)
{
	static char*	last = nil;
	char*	c;
	int	fd, port;
	char	kaddr[50];

	fd = open(kbdctl, OWRITE);
	if (fd < 0){
		fprint(2, "mousefs: can't redirect kbd: %r\n");
		return;
	}
	if (last && !islocal(last)){
		dprint(2, "mousefs: kbd: close %s\n", last);
		fprint(fd, "close %s\n", last);
	}
	free(last);
	last = nil;
	strecpy(kaddr, kaddr+sizeof(kaddr), addr);
	c = strrchr(kaddr, '!');
	if (c){
		*c = 0;
		port = atoi(c+1) + 1;
		seprint(c, kaddr+sizeof(kaddr), "!%d", port);
	}
	dprint(2, "mousefs: kbd: call %s\n", kaddr);
	if (fprint(fd, "call %s\n", kaddr) < 0)
		dprint(2, "mousefs: kbd call failed: %r\n");
	last = strdup(kaddr);
	close(fd);
}

static void
local(void)
{
	dprint(2, "local\n");
	changecursor(0);
	inm = outm = 0;
	kbdsetdest("local");
}

static void
reclaim(void)
{
	if (inm == 1 || outm == 1)
		sendp(devs[1].out, "bye");
	local();
	while(devs[1].sender || devs[1].receiver){
		sleep(500);
		yield();
	}
}

static void
outgoing(void)
{
	dprint(2, "outgoing\n");
	changecursor(1);
	inm = 0;
	outm= 1;
	kbdsetdest(devs[1].addr);
}

static void
incoming(void)
{
	dprint(2, "incoming\n");
	changecursor(0);
	inm = 1;
	outm= 0;
	kbdsetdest("local");
}

static void
sendproc(void* a)
{
	Dev*	d = a;
	char*	e;

	threadsetname("sendproc");
	dprint(2, "sendproc %p starting\n", d);
	while(e = recvp(d->out)){
		if (strncmp(e, "bye", 3) == 0){
			dprint(2, "sendproc %p: bye\n", d);
			write(d->con, "bye", 3);
			continue;
		}
		if (strncmp(e, "call ", 5) == 0){
			write(d->con, e, strlen(e));
			free(e);
			continue;
		}
		if (strlen(e) != Mevlen){
			dprint(2, "sendproc %p: bad event: %s\n", d, e);
		} else if (e[0] == 'm'){
			e[1] = ' ';
			if (d != &devs[0])
				htonmouse(e);
			Dprint(2, "sent to %p %s\n", d, e);
			if (write(d->con, e, Mevlen) != Mevlen){
				fprint(2, "%s: write: %r\n", argv0);
				free(e);
				write(d->con, "bye", 3);
				break;
			}
		}
		free(e);
	}
	write(d->con, "bye", 3);
	write(d->cfd, "hangup", 6);
	close(d->cfd);
	d->cfd = -1;
	d->addr[0] = 0;
	d->sender = 0;
	dprint(2, "sendproc %p exiting\n", d);
	threadexits(nil);
}

static void
recvproc(void* a)
{
	char	buf[Mevlen+1];
	Dev*	d;
	int	n;
	char*	caddr;

	d = a;
	threadsetname("recvproc");
	dprint(2, "recvproc %p starting\n", d);
	caddr = nil;
	for(;;){
		n = read(d->con, buf, Mevlen);
		if (n == 0){
			dprint(2, "recvproc %p: eof\n", d);
			break;
		}
		if (n < 0){
			dprint(2, "%s: recvproc %p: %r\n", argv0, d);
			break;
		}
		buf[n] = 0;
		Dprint(2, "msread: %s\t%p i=%p o=%p\n", buf, d, &devs[inm], &devs[outm]);
		if (!strncmp(buf, "bye", 3)){
			dprint(2, "recvproc %p: bye\n", d);
			break;
		}
		if (!strncmp(buf, "call ", 5)){
			dprint(2, "recvproc %p: call\n", d);
			caddr = buf + 5;
			break;
		}
		if (d == &devs[0]){
			if (buf[0] == 'r'){
				getsizes();
			}
			if (isreclaim(buf)){
				dprint(2, "mousefs: reclaim\n");
				reclaim();
				continue;
			}
			Dprint(2, "mouse %p send: %s\n", d, buf);
			buf[1] = d->name;
			if (outm == 0){
				if (readers.ref > 0)
					sendp(devs[outm].evs, estrdup9p(buf));
			} else
				if (buf[0] == 'm')
					sendp(devs[outm].out, estrdup9p(buf));
		} else {
			ntohmouse(buf);
			Dprint(2, "mouse %p deliver: %s\n", d, buf);
			buf[1] = d->name;
			sendp(devs[0].out, estrdup9p(buf));
			if (readers.ref > 0)
				sendp(devs[0].evs, estrdup9p(buf));
		}
	}
	sendp(d->out, nil);
	write(d->con, "bye", 3);
	write(d->cfd, "hangup", 6);
	close(d->cfd);
	close(d->con);
	d->con = -1;
	dprint(2, "recvproc %p exiting\n", d);
	local();
	d->receiver = 0;
	if (caddr != nil && 0){	// not safe. Still too easy to make loops
		reclaim();
		if (newpeer(caddr))
			outgoing();
	}
	threadexits(nil);
}

static void
initdev(Dev* dev, char name, char* addr)
{
	dev->evs = chancreate(sizeof(char*), Nevs);
	dev->out = chancreate(sizeof(char*), Nevs);
	dev->name = name;
	dev->con = -1;
	dev->cfd = -1;
	if (addr)
		strecpy(dev->addr, dev->addr+sizeof(dev->addr), addr);
}

static Dev*
newdev(char* addr, int fd)
{
	Dev*	dev;

	if (islocal(addr)){	// allowed just once
		dev = &devs[0];
		if (dev->con >= 0)
			return nil;
	} else
		dev = &devs[1];
	strecpy(dev->addr, dev->addr+sizeof(dev->addr), addr);
	dev->con = fd;
	dev->cfd = -1;
	dev->receiver = dev->sender = 1;
	procrfork(recvproc, dev, Stack, RFNAMEG|RFNOTEG);
	procrfork(sendproc, dev, Stack, RFNAMEG|RFNOTEG);
	dprint(2, "mousefs: newdev %s\n", addr);
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
		case Qmouse:
			incref(&readers);
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
		d->qid.path = Qmouse;
		d->name = estrdup9p("mouse");
		break;
	case 1:
		d->mode = 0220;
		d->qid.path = Qctl;
		d->name = estrdup9p("mousectl");
		break;
	default:
		return -1;
	}
	return 0;
}

void
mreadproc(void* a)
{
	static char	locked = 0;
	char*	ev;
	int	b;
	Req*	r;
	Channel* mreadreq = a;

	threadsetname("mreadproc");
	for(;;){
		r = recvp(mreadreq);

		/* Merging mouse streams may cause pressed buttons to
		 * be `released' because of events that come from other
		 * mouses. To prevent this, a mouse that is holding a
		 * button down is locking the mouse (other ones ignored).
		 * A gone device releases the lock as well.
		 */
		for(;;){
			ev = recvp(devs[0].evs);
			if (ev == nil){
				fprint(2, "%s: nil event?", argv0);
				continue;
			}
			if (strlen(ev) != Mevlen){
				fprint(2,"%s: bad mouse event: [%s]\n", argv0, ev);
				continue;
			}
			if (!locked || ev[1] == locked)
				break;
			free(ev);
		}
		b = (ev[1+11+1+11+1+10] != '0');
		if (locked && locked == ev[1] && !b){
			locked = 0;
		} else if (!locked && b){
			locked = ev[1];
		}
		ev[1] = ' ';
		r->ofcall.count = strlen(ev);
		if (r->ifcall.count < r->ofcall.count)
			r->ofcall.count = r->ifcall.count;
		memmove(r->ofcall.data, ev, r->ofcall.count);

		Dprint(2, "fsread: %s\n", ev);
		free(ev);
		respond(r, nil);
	}
}

static void
fsread(Req *r)
{
	static Channel* mreadreqc = nil;
	static char	locked = 0;
	char	m[200];
	char*	e;
	int	p;
	int	i;

	p = r->fid->qid.path;
	switch(p){
	case Qdir:
		dirread9p(r, dirgen, nil);
		break;
	case Qmouse:
		/* We can't service the read ourselves. If we block
		 * then any request on the fs would have to wait.
		 * This is a BAD thing because we bind MBEFORE at /dev.
		 */
		if (mreadreqc == nil){
			mreadreqc = chancreate(sizeof(Req*), Nevs);
			proccreate(mreadproc, mreadreqc, Stack);
		}
		sendp(mreadreqc, r);
		return;
	case Qctl:
		e = m;
		lock(&devslock);
		for (i = 0; i < nelem(devs); i++)
			if (devs[i].addr[0])
				e = seprint(e, m+sizeof(m), "%s\n", devs[i].addr);
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
	i = auth_proxy(fd, nil, "proto=p9any user=%s role=%s", getuser(), role);
	alarm(0);
	if (i == nil)
		return 0;
	auth_freeAI(i);
	return 1;
}

static int
newpeer(char* addr)
{
	int	fd;
	int	cfd;
	Dev*	d;
	char*	na;

	na = netmkaddr(addr, "tcp", "mouse");
	if (islocal(addr) || islocal(na)){
		dprint(2, "%s: newpeer: local addr: %s\n", argv0, addr);
		return 0;
	}
	fd = dial(na, 0, 0, &cfd);
	if (fd < 0){
		fprint(2, "mousefs: dial %s: %r\n", addr);
		return 0;
	}
	if (doauth && !authfd(fd, "client")){
		fprint(2, "mousefs: can't auth to %s: %r\n", addr);
		close(fd);
		close(cfd);
		return 0;
	}
	if (d = newdev(addr, fd)){
		d->cfd = cfd;
		fprint(d->cfd, "keepalive 2000");
	}
	return d != nil;
}

static char*
ctl(char* msg)
{
	char*	args[4];
	int	nargs;
	char*	addr;
	char	call[Mevlen];
	char*	na;

	/* call address
	 * close address
	 */
	nargs = tokenize(msg, args, nelem(args));
	if (nargs != 2)
		return Ebadctl;
	if (strcmp(args[0], "call") == 0){
		addr = csquery(args[1]);
		if (addr == nil){
			reclaim();
			return Enotexist;
		}
		if (inm == 0){
			reclaim();
			if (!newpeer(addr)){
				free(addr);
				return Enotexist;
			}
			outgoing();
		} else {
			na = netmkaddr(addr, "tcp", "11000");
			if (!islocal(addr) && !islocal(na)){
				seprint(call,call+sizeof(call),"call %s",args[1]);
				nbsendp(devs[inm].out, estrdup9p(call));
				reclaim();
			}
		}
		free(addr);
	} else if (strcmp(args[0], "close") == 0){
		reclaim();
	} else if (strcmp(args[0], "debug") == 0){
		debug = atoi(args[1]);
	} else {
		return Ebadctl;
	}
	return nil;
}

static void
fswrite(Req *r)
{
	static int mctl = -1;
	int	p;
	char	msg[80];
	int	n;
	char*	e;
	p = r->fid->qid.path;
	if (mctl < 0)
		mctl = open("#m/mousectl", OWRITE|OCEXEC);
	switch(p){
	case Qmouse:
		n = r->ifcall.count;
		if (n > sizeof(msg)-1)
			n = sizeof(msg)-1;
		memmove(msg, r->ifcall.data, n);
		msg[n] = 0;
		r->ofcall.count = n;
		Dprint(2, "fswrite: %s\n", msg);
		fstohmouse(msg);
		sendp(devs[0].out, estrdup9p(msg));
		if (inm != 0)
			sendp(devs[inm].out, estrdup9p(msg));
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
			if (strcmp(e, Ebadctl) == 0){
				if(write(mctl, r->ifcall.data, n) < 0){
					respond(r, Ebadctl);
					return;
				}
			} else {
				respond(r, e);
				return;
			}
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
	} else if (strcmp(name, "mouse") == 0){
		fid->qid.path = Qmouse;
		fid->qid.type = 0;
	} else if (strcmp(name, "mousectl") == 0){
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
	case Qmouse:
		dirgen(0, &r->d, nil);
		break;
	case Qctl:
		dirgen(1, &r->d, nil);
		break;
	}
	respond(r, nil);
}

static void
fsclunk(Fid* f)
{
	if (f->omode >= 0 && f->qid.path == Qmouse)
		decref(&readers);
}

static Srv mfs=
{
.attach=fsattach,
.open=	fsopen,
.read=	fsread,
.stat=	fsstat,
.write= fswrite,
.walk1= fswalk1,
.destroyfid= fsclunk,
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
	afd = announce(addr, adir);
	if (afd < 0)
		sysfatal("can't announce: %r");
	for(;;){
		lfd = listen(adir, ldir);
		if (lfd < 0)
			sysfatal("can't listen: %r");
		reclaim();
		dfd = accept(lfd, ldir);
		i = getnetconninfo(ldir, dfd);
		if (doauth && !authfd(dfd, "server")){
			fprint(2, "auth failed for %s\n", i->rsys);
			close(lfd);
			close(dfd);
		} else if (dev = newdev(i->rsys, dfd)){
			dev->receiver = dev->sender = 1;
			dev->cfd = lfd;
			fprint(dev->cfd, "keepalive 2000");
			proccreate(recvproc, dev, Stack);
			proccreate(sendproc, dev, Stack);
			dprint(2, "mousefs: call from %s\n", dev->addr);
			incoming();
		} else {
			close(lfd);
			fprint(2, "local call ignored\n");
		}
		freenetconninfo(i);
	}
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

void
srvproc(void* a)
{
	int*	p = a;

	threadsetname("srvproc");
	getsizes();
	mousefd = open("/dev/mouse", ORDWR|OCEXEC);
	if (mousefd < 0)
		sysfatal("can't open mouse: %r");
	initdev(&devs[0], 'a', localaddr);
	initdev(&devs[1], 'b', nil);
	//setpri(13);
	newdev(localaddr, mousefd);
	proccreate(listener, estrdup9p(addr), Stack);
	threadnotify(alarmed, 1);
	close(p[1]);
	mfs.infd = p[0];
	mfs.outfd= p[0];
	mfs.nopipe= 1;
	mfs.srvfd = -1;
	srv(&mfs);
	dprint(2, "mousefs: srvproc exiting\n");
	threadexitsall(nil);
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
	fprint(2, "usage: %s [-AdD] [-m mnt] [-n addr]\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char **argv)
{
	char*	mnt;
	int	p[2];

	mnt = "/dev";
	addr= "tcp!*!11000";
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
	addr = netmkaddr(addr, "tcp", "11000");
	localaddr = csquery(addr);

	if(pipe(p) < 0)
		sysfatal("pipe: %r");
	procrfork(srvproc, p, Stack, RFNAMEG|RFFDG|RFNOTEG);
	close(p[0]);
	if (mount(p[1], -1, mnt, MBEFORE, "") < 0)
		sysfatal("mount: %r");
	if (addr != nil && vname != nil)
		proccreate(announceproc, 0, 8*1024);
	threadexits(nil);
}
