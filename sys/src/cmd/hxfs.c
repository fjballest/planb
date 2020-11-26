#include <u.h>
#include <libc.h>
#include <b.h>
#include <thread.h>
#include <fcall.h>
#include <auth.h>
#include <9p.h>
#include <bio.h>

typedef struct Pos Pos;
typedef struct Loc Loc;
typedef struct Badge Badge;
typedef struct Name Name;

enum {
	Qdir,
	Qbadge,

	Ntoks	= 32,
	Interval= 2,	// poll interval, in seconds.
	Nlocs	= 3,	// # of locations kept per badge
	Nbadges	= 16,	// Max # of badges
	Nnames	= 128,	// Max # of names (badges+locations)
	Troam	= 5,	// time (secs) to forget old location
	Told	= 120,	// time (secs) to forget last location
	Tdial	= 15,	// time (secs) to retry a dial for srv.
	Stack	= 16 * 1024,

	Esc	= 27,

};

struct Pos {
	int	badge;
	int	loc;
};

struct Loc {
	int	loc;
	int	time;
};

struct Badge {
	int	id;
	Loc	locs[Nlocs];
};

struct Name {
	int	id;
	char*	name;
};

#define dprint	if(debug)fprint
static int	debug;
static char*	ctladdr;
static Channel*	posc;
static Channel*	fsc;
static Badge	badges[Nbadges];
static Name*	bnames[Nnames];
static int	nbnames;
static Name*	mnames[Nnames];
static int	nmnames;
static File*	slash;
static int	iofd = -1;
static char*	addr;
static char*	vname;

int mainstacksize = 32*1024;

int
fileclass(File* fp)
{
	return ((fp->qid.path&0xff000000)>>24);
}

/* A badge may be seen by several receivers, and
 * may be at several locations. It may be unnoticed
 * for a while, so it takes a while to assume that
 * its last location is no longer valid.
 * We keep the last Nlocs locations for each badge.
 * We forget those older than Told secs.
 *
 * Badges and locations are added into the data
 * structure and never removed. Files for badges keep
 * a pointer into the corresponding badge. Their content
 * appears to be the set of active locations (most recent first)..
 */

static void
addname(int id, char* s)
{
	Name*	n;

	n = emalloc9p(sizeof(Name));
	n->id = id;
	n->name= s;

	dprint(2, "addname: %d %s\n", id, s);
	if (id < 1000){
		if (nbnames >= nelem(bnames))
			sysfatal("too many names");
		bnames[nbnames++] = n;
	} else {
		if (nmnames >= nelem(mnames))
			sysfatal("too many names");
		mnames[nmnames++] = n;
	}
}


static char*
id2name(int id)
{
	int	i;
	Name**	n;
	int	nn;

	if (id < 1000){
		n = bnames;
		nn= nbnames;
	} else {
		n = mnames;
		nn= nmnames;
	}
	for (i = 0; i < nn; i++)
		if (n[i]->id == id)
			return n[i]->name;
	return nil;
}

static void
dumpnames(void)
{
	int	i;

	fprint(2, "badges:\n");
	for (i = 0; i < nbnames; i++)
		fprint(2, "0x%x -> %s\n", bnames[i]->id, bnames[i]->name);
	fprint(2, "monitors:\n");
	for (i = 0; i < nmnames; i++)
		fprint(2, "0x%x -> %s\n", mnames[i]->id, mnames[i]->name);
}

static void
dumpbadges(void)
{
	int	i, j;
	char*	n;

	fprint(2, "locations:\n");
	for (i = 0; i < nelem(badges); i++)
		if (badges[i].id){
			n = id2name(badges[i].id);
			if (n == nil)
				continue;
			fprint(2,"\t %s:\t", n);
			for (j = 0; j < Nlocs; j++){
				if (badges[i].locs[j].time){
					n = id2name(badges[i].locs[j].loc);
					if (n)
						fprint(2, " %s", n);
					else
						fprint(2, " unknown");
				}
			}
			fprint(2,"\n");
		}
	fprint(2,"\n");
}

static void
fsread(Req* r)
{
	File* file;
	int	id;
	Badge*	b;
	char	buf[80];
	char*	s;
	int	i;
	char*	n;

	if (r->fid->qid.type&QTAUTH){
		authread(r);
		return;
	}
	file = r->fid->file;
	id = fileclass(file);
	switch(id){
	case Qbadge:
		rlock(file);
		b = file->aux;
		assert(b);
		s = buf;
		for (i = 0; i < Nlocs; i++){
			if (b->locs[i].loc){
				n = id2name(b->locs[i].loc);
				if (n)
					s = seprint(s, buf+80, "%s\n", n);
				else
					s = seprint(s, buf+80, "unknown\n");
			}
		}
		if (s == buf)
			strcpy(buf, "none\n");
		runlock(file);
		readstr(r, buf);
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
	int	mode;

	if (r->fid->qid.type&QTAUTH)
		sysfatal("fsopen for an AUTH file. fixme: add respond/return\n");
	mode = (r->ifcall.mode&3);
	if (mode != OREAD)
		respond(r, "permission denied");
	else
		respond(r, nil);
}

static void
fswrite(Req* r)
{
	if (r->fid->qid.type&QTAUTH)
		authwrite(r);
	else
		respond(r, "permission denied");
}

static void
fsclean(Srv* s)
{
	Channel* rc;

	rc = s->aux;
	if (rc)
		chanfree(rc);
}
	
static void
fssend(Req* r)
{
	Channel* rc;

	
	if (r->srv->aux == nil)
		r->srv->aux = chancreate(sizeof(ulong), 0);
	rc = r->srv->aux;
	sendp(fsc, r);
	recvul(rc);
}

static void
fsclunk(Fid* fid)
{

	if (fid->qid.type&QTAUTH)
		authdestroy(fid);
}

static Srv sfs=
{
	.auth	=auth9p,
	.attach	=fsattach,
	.open	=fsopen,
	.write = fswrite,
	.read  = fssend,
	.end	= fsclean,
	.destroyfid = 	fsclunk,
};

static int
loccmp(void* l1, void* l2)
{
	Loc* l1p = l1;
	Loc* l2p = l2;

	return l2p->time - l1p->time;
}

static void
sortlocs(Badge* b)
{
	qsort(b->locs, Nlocs, sizeof(b->locs[0]), loccmp);
}

static void
collectold(void)
{
	int	i, j;
	long	now;
	long	dt;

	now = time(nil);
	for (i = 0; i < nelem(badges); i++)
		if (badges[i].id){
			for (j = 0; j < Nlocs; j++){
				if (badges[i].locs[j].time){
					dt = now - badges[i].locs[j].time;
					if (j > 0 && dt > Troam){
						badges[i].locs[j].time = 0;
						badges[i].locs[j].loc = 0;
					}
					if (j == 0 && dt > Told){
						badges[i].locs[j].time = 0;
						badges[i].locs[j].loc = 0;
					}
				}
			}
			sortlocs(badges+i);
		}
}

static void
updatebadge(Badge* b, int loc)
{
	int	old;
	int	i;
	long	now;

	now = time(nil);
	for (old = i = 0; i < Nlocs; i++){
		if (b->locs[i].loc == loc){
			old = i;
			break;
		}
		if (b->locs[old].time > b->locs[i].time)
			old = i;
	}
	b->locs[old].time = now;
	b->locs[old].loc = loc;
	sortlocs(b);
}

static void
addfile(Badge* b)
{
	File*	f;
	char*	n;

	n = id2name(b->id);
	if (n){
		f = createfile(slash, n, "sys", 0444, b);
		f->qid.path |= ((Qbadge&0xff)<<24);
		closefile(f);
	}
}

static void
updatepos(Pos* p)
{
	int	i;

	for (i = 0; i < nelem(badges); i++){
		if (badges[i].id == p->badge){
			updatebadge(badges+i, p->loc);
			return;
		}
	}
	for (i = 0; i < nelem(badges); i++){
		if (!badges[i].id){
			badges[i].id = p->badge;
			updatebadge(badges+i, p->loc);
			addfile(badges+i);
			return;
		}
	}
	fprint(2, "%s: too many badges; ignoring %x\n", argv0, p->badge);
}

static void
parsepos(char* s)
{
	char	mname[] = "0xXXXX";
	char	bname[] = "0xXXXX";
	char*	p;
	Pos	pos;

	p = strchr(s, ' ');
	if (p == nil || p - s != 4){
		dprint(2, "bad monitor name: %s\n", s);
		return;
	}
	*p++ = 0;
	strcpy(mname + 2, s);
	pos.loc = strtol(mname, nil, 0);
	while(p && *p){
		s = p;
		p = strchr(s, ' ');
		if (p == nil)
			break;
		*p = 0;
		if (p - s < 6){
			dprint(2, "bad badge name: %s\n", s);
			break;
		}
		p++;
		strncpy(bname + 2, s, 4);
		pos.badge = strtol(bname, nil, 0);
		dprint(2, "badge %d at %d\n", pos.badge, pos.loc);
		send(posc, &pos);
	}
}

static int
hxcmd(int fd, char cmd, char ans)
{
	char	buf[1];
	int	i, n;

	i = 0;
	do {
		if (write(fd, &cmd, 1) != 1)
			return 0;
		if (ans == 0)
			break;
		n = read(fd, buf, sizeof(buf));
		if (n <= 0)
			return 0;
		if (buf[0] != ans){
			if(0)dprint(2, "%s: cmd %c: bad answer '%c' 0x%x\n",
				argv0, cmd, buf[0], buf[0]);
			continue;
		}
	} while (buf[0] != ans && i++ < 100);
	if (i == 100){
		werrstr("HX5C not responding");
		return 0;
	}
	return 1;
}

static void
hxinit(int fd)
{
	hxcmd(fd, Esc, 0);
}

static uchar*
PUTS(uchar* p, int s, int* sum)
{
	p[0] = ((s >> 8) & 0xff);
	*sum = ((*sum + p[0]) & 0xff);
	p[1] = (s & 0xff);
	*sum = ((*sum + p[1]) & 0xff);
	return p + 2;
}

static uchar*
PUTB(uchar* p, int s, int* sum)
{
	p[0] = (s & 0xff);
	*sum = ((*sum + p[0]) % 256);
	return p + 1;
}

static void
dumpbuf(uchar* buf, int n)
{
	int	i;

	for (i = 0; i < n; i++)
		print("%c ", buf[i]);
	print("\n");
}

static int
hxconfig(int fd)
{
	uchar	buf[4096];
	uchar*	p;
	int	sum;
	int	i;
	char	cmd;

	cmd = '$';
	hxcmd(fd, cmd, '!');
	sum = 0;
	p = buf;
	p = PUTS(p, 2*nmnames + 1, &sum);	// size
	p = PUTB(p, Interval, &sum);
	for (i = 0; i < nmnames; i++)
		p = PUTS(p, mnames[i]->id, &sum);
	p = PUTB(p, sum, &sum);

	dprint(2, "wrote: ");
	if (debug)
		dumpbuf(buf, p-buf);
	dprint(2, "\n\n");
	if (write(fd, buf, p - buf) != p - buf)
		sysfatal("write: %r\n");
	read(fd, buf, 1);
	if (buf[0] != '+')
		return 0;
	return 1;
}

static void
consume(char* buf)
{
	char*	toks[Ntoks];
	char	delims[2];
	int	ntoks;
	int	i;
	int	l;

	delims[0] = 0xd;
	delims[1] = 0;
	ntoks = gettokens(buf, toks, nelem(toks), delims);
	for (i = 0; i < ntoks; i++)
		if (toks[i] && *toks[i]){
			l = strlen(toks[i]);
			if (toks[i][l-1] != '#' || l < 5)
				continue;
			if(0)dprint(2, "got '%s'\n", toks[i]);
			toks[i][l-1] = 0;
			parsepos(toks[i]);
		}
}

static void
hxio(void* )
{
	uchar	buf[1024];
	int	n;
	int	nr;

	threadsetname("hxio");
	n = 0;
	for(;;){
		nr = read(iofd, buf + n, sizeof(buf) - n);
		if (nr <= 0)
			sysfatal("read: %r\n");
		if (0 && debug)
			dumpbuf(buf, nr);
		n += nr;
		if (n >= sizeof(buf)-1 || buf[n-1] == 0x0d){
			buf[n] = 0;
			consume((char*)buf);
			n = 0;
		}
	}
	threadexits(nil);
}

static int
hxopen(char* file)
{
	int	fd;
	char*	cfname;
	int	cfd;

	fd = open(file, ORDWR);
	if (fd < 0)
		fprint(2, "cant' open %s: %r\n", file);
	cfname = smprint("%sctl", file);
	assert(cfname);
	cfd = open(cfname, OWRITE);
	if (cfd >= 0){
		fprint(cfd, "b19200 l8 pn s1\n");
		close(cfd);
	}
	free(cfname);
	return fd;
}

static void
io(void*)
{
	Channel*rc;
	Pos	p;
	Req*	r;
	Alt	a[] = {
		{ posc, &p, CHANRCV },
		{ fsc,  &r, CHANRCV },
		{ 0, 0, CHANEND }};

	threadsetname("hxio");
	for(;;){
		switch(alt(a)){
		case 0:
			updatepos(&p);
			if (debug)
				dumpbadges();
			collectold();
			break;
		case 1:
			rc = r->srv->aux;
			fsread(r);
			sendul(rc, 0);
			break;
		default:
			sysfatal("io: recv");
		}
	}
	threadexits(nil);
}

/*
 * The file contains words of the form:
 *	id=name
 * Usually, one per line.
 * No comments or any other fancy stuff.
 */
static void
config(char* fname)
{
	char*	cfg;
	char*	toks[Nnames];
	int	ntoks;
	char*	s;
	int	i;

	cfg = readfstr(fname);
	if (!cfg)
		sysfatal("can't read config");
	ntoks = tokenize(cfg, toks, nelem(toks));
	for (i = 0; i < ntoks; i++){
		s=strchr(toks[i], '=');
		if (s){
			*s++ = 0;
			addname(strtol(toks[i], nil, 10), s);
		}
	}
	// We leak cfg because addname takes the given string
	// and does not make a dup. This is more simple.
}

static void
announceproc(void*)
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
	fprint(2, "usage: %s [-Ad] [-s srv] [-m mnt] [-n addr] [-c cfg] [-V vol] [iofile]\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char **argv)
{
	char*	mnt;
	char*	srv;
	char*	cfg;
	int	i;

	srv = nil;
	mnt = nil;
	addr = nil;
	cfg = "/sys/lib/hxconf";
	ARGBEGIN{
	case 'A':
		sfs.auth = nil;
		break;
	case 'd':
		if (debug++)
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
	case 'c':
		cfg =  EARGF(usage());
		break;
	case 'V':
		vname = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;

	switch(argc){
	case 0:
		ctladdr = "/dev/eia0";
		break;
	case 1:
		ctladdr = argv[0];
		break;
	default:
		usage();
	}
	config(cfg);
	if (debug)
		dumpnames();
	else
		rfork(RFNOTEG);

	iofd = hxopen(ctladdr);
	if (iofd < 0)
		sysfatal("hxopen");
	for (i = 0; i < 3; i++){
		hxinit(iofd);
		if (hxconfig(iofd))
			break;
		sleep(1000);
	}
	if (i == 3)
		sysfatal("can't config hx");

	if (getenv("local"))
		sfs.auth = nil;
	if (srv == nil && mnt == nil && addr == nil)
		addr="tcp!*!11008";
	sfs.tree =  alloctree(nil, nil, DMDIR|0555, nil);
	slash = sfs.tree->root;
	incref(slash);
	posc = chancreate(sizeof(Pos), 5);
	fsc = chancreate(sizeof(Req*), 5);
	proccreate(io, nil, Stack);
	proccreate(hxio, nil, Stack);
	if (addr != nil)
		threadlistensrv(&sfs, addr);
	if (srv != nil || mnt != nil)
		threadpostmountsrv(&sfs, srv, mnt, MREPL|MCREATE);
	if (addr != nil && vname != nil)
		proccreate(announceproc, 0, 8*1024);
	threadexits(nil);
}
