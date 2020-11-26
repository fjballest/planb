/*
 * Plan B volume file system server.
 *
 * Is Plan B safe?
 *
 * When used as directed, Plan B is safe for most women.  There have been
 * no serious complications associated with Plan B. Common side effects
 * include nausea, abdominal pain, fatigue, headache, and menstrual
 * changes.  Women who are pregnant, have undiagnosed vaginal bleeding,
 * or have an allergy to the product should not use Plan B. Plan B cannot
 * terminate an established pregnancy
 *
 * How effective is Plan B?
 *
 * Taken within 72 hours of unprotected intercourse, Plan B can, when
 * used correctly, reduce the risk of pregnancy by 89 percent after a
 * single act of unprotected sex.  Effectiveness declines as the interval
 * between intercourse and the start of treatment increases.
 *
 * http://www.go2planb.com
 *
 * How to use Plan B?
 *
 * This replaces the old vold (with the discovery protocol)
 * and the kernel volume mechanism.
 *
 * When supplied a volume spec it provides
 * a file tree that corresponds to a volume instance, which one,
 * it depends on the set of volumes known.
 *
 * This translates the client RPCs to RPCs for the appropriate fs.
 * Using the local kernel as a client was beareable but a bit slow.
 *
 * 5.48u 4.30s 49.72r 	 mk at /sys/src/9/pc volfs
 * 5.45u 5.40s 43.68r 	 mk at /sys/src/9/pc planb 3rd ed.
 */


#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <bio.h>
#include <ip.h>
#include <thread.h>
#include <pool.h>
#include <auth.h>
#include "names.h"
#include "vols.h"

typedef struct Work Work;

struct Work {
	Frpc*	rpc;	// being serviced
	int	nrpcs;	// serviced
	Channel*c;	// where to send requests
};


int	debug, vdebug; /* general debug (xtra if > 1); volume debug */

static int	srvfd = -1;
static int	ctlfd = -1;
static Channel*	reqc;
static Channel*	repc;
static Channel*	tmrc;

static char*	sname;
static char*	csname;

static int	dodiscover;

static Work	work[Nthreads];
static char*	adsaddr;

/* When feasible, we use the client's Frpc to issue all
 * our rpcs to the server. This means that some routines
 * in volfs.c and in vols.c receive an fop parameter.
 * (A benefit of this is that we can use the client tags
 * as our tags for RPCs made to servers. Another benefit
 * is that we avoid extra allocation of Frpcs.)
 * Beware that such routines will overwrite whatever
 * is in the fop to issue appropriate rpcs. However,
 * tag is guaranteed to remain untouched.
 */
static QLock	rpclck;
static Frpc*	freerpcs;
static int	nrpcs;	// #rpcs allocated. 

Frpc*
rpcalloc(void)
{
	int	x[1];
	Frpc*	m;
	Channel*sc;

	qlock(&rpclck);
	if (freerpcs == nil){
		nrpcs++;
		m = emalloc(sizeof(Frpc));
		setmalloctag(m, getcallerpc(x+2));
		memset(m, 0, sizeof(Frpc));
		m->sc = chancreate(sizeof(ulong), 0);
	} else {
		m = freerpcs;
		freerpcs = m->next;
		sc = m->sc;
		memset(m, 0, sizeof(Frpc));
		m->sc = sc;
	}
	if (nrpcs > Nrpcs && !(nrpcs%20))
		fprint(2, "%s: warning: #%d allocated rpcs\n", argv0, nrpcs);
	qunlock(&rpclck);
	return m;
}

void
rpcfree(Frpc* m)
{
	if (m == nil)
		return;
	if (m->time == 0xbbbb){
		fprint(2, "rpcfree: double free for %p\n", m);
		abort();
	}
	m->time = 0xbbbb;
	rpcfree(m->rep);
	m->rep = nil;
	qlock(&rpclck);
	free(m->d);
	m->d = nil;
	m->next = freerpcs;
	freerpcs = m;
	qunlock(&rpclck);
}

static void
flushtag(int tag)
{
	static int nflushed;
	int	i;

	for (i = 0; i < nelem(work); i++){
		if (work[i].rpc && work[i].rpc->f.tag == tag)
			break;
	}
	if (i == nelem(work)){
		dprint(2, "flush: no tag %d (%d flushes)\n", tag, nflushed++);
		return;
	}
	work[i].rpc->flushed = 1;
	work[i].rpc->time = 0;
}

static void
workthread(void* a)
{
	Work*	w = a;
	Frpc*	m;

	threadsetname("work");
	while(m = recvp(w->c)){
		w->nrpcs++;
		assert(m->f.type >= 0 && m->f.type < Tmax);
		assert(fscalls[m->f.type]);

		w->rpc = m;
		fscalls[m->f.type](m);
		if (m->f.type == Tflush)
			flushtag(m->f.oldtag);
		sendp(repc, m);
		w->rpc = nil;
	}
	threadexits(nil);
}

static void
replythread(void *)
{
	Frpc*	m;
	Fid*	df;

	threadsetname("replythread");
	df = emalloc(sizeof(Fid));
	while(m = recvp(repc)){
		if (m->fid && m->fid->debug)
			df->debug = m->fid->debug;
		else
			df->debug = 0;
		fidfree(m->freefid, m);
		if (m->flushed){
			dprint(2, "flushed reply (type %d, tag %d) ignored\n",
				m->r.type, m->r.tag);
		} else {
			m->fid = df;
			if (putfrep(srvfd, m) < 0)
				fprint(2, "%s: reply: %r\n", argv0);
		}
		rpcfree(m);
	}
	sysfatal("no output process");
}

static void
discover(char* addr)
{
	static char*	raddr = nil;
	static int	fd = -1;
	char	dir[50];
	NetConnInfo*ni;

	if (fd < 0)
		if (raddr != nil)
			fd = dial(netmkaddr(raddr, nil, "ads"), 0, 0, 0);
		else
			fd = dial(netmkaddr(addr, nil, "ads"), 0, dir, 0);
	if (fd < 0){
		fprint(2, "%s: %s: %r\n", argv0, addr);
	} else {
		if (raddr == nil){
			ni = getnetconninfo(dir, fd);
			if (ni != nil){
				raddr = estrdup(ni->raddr);
				free(ni);
			}
		}
		// This print is ignored by old protocol
		// the new one uses it as a request to dump announces.
		if (fprint(fd, "PlanB announces\n") < 0){
			close(fd);
			fd = -1;
			// We'll try reopening next time they call us.
		} else
			fdconfig(fd, addr);
	}
}


static void
timerthread(void*)
{
	ulong now;
	ulong last;

	threadsetname("timers");
	last = 0;
	for(;;){
		now = recvul(tmrc);
		fstimer(now);
		if (dodiscover && adsaddr && now - last > Disctick){
			discover(adsaddr);
			last = time(nil);
		}
	}
}

static void
stats(void)
{
	int	i;
	int	nused;

	fprint(2, "%d rpcs allocated %d workers ", nrpcs, nelem(work));
	nused = 0;
	for (i = 0; i < nelem(work); i++){
		if (work[i].nrpcs)
			nused++;
	}
	fprint(2, "%d idle\n", nelem(work) - nused);
}

static void
ctlproc(void*)
{
	static Biobuf	bcmd;
	char*	ln;
	char*	c;

	threadsetname("ctlproc");
	Binit(&bcmd, ctlfd, OREAD);
	while(ln = Brdstr(&bcmd, '\n', 1)){
		switch(ln[0]){
		case 'V':
			fprint(2, "Debug dump:\n");
			dumpvols();
			dumpfss();
			dumpmvols();
			dumpfids();
			break;
		case 'S':
			fprint(2, "Debug dump:\n");
			stats();
			fsstats();
			break;
		case 'D':
			debug = vdebug = 0;
			for(c = ln+1; *c; c++)
				switch(*c){
				case 'n':
					break;
				case 'D':
					debug = 2;
					break;
				case 'd':
					debug = 1;
					break;
				case 'v':
					vdebug = 1;
					break;
				}
			break;
		case 'T':
			c = strchr(ln+1, ' ');
			if (c)
				*c = 0;
			c = strchr(ln+1, '\n');
			if (c)
				*c = 0;
			if (ln[1] == 'T')
				tracemvol(ln+2, 2);
			else
				tracemvol(ln+1, 1);
			break;
		case 'N':
			tracemvol(nil, 0);
			break;
		default:
			cmdline(ln, "/srv/volctl", 0);
		}
		free(ln);
	}
	Bterm(&bcmd);
	exits(nil);
}

static void
dispatchproc(void*)
{
	Frpc*	m;
	int	i;
	int	rr;
	Frpc*	fop;

	threadsetname("dispatcher");
	for (i = 0; i < Nthreads; i++)
		threadcreate(workthread, work+i, Wstack);
	threadcreate(replythread, nil, Stack);
	threadcreate(timerthread, nil, Stack);
	rr = 0;
	fop = rpcalloc();
	fop->f.tag = 1;
	while(m = recvp(reqc)){
		checkvols(fop);
		for (i = 0; i < Nthreads; i++){
			if (work[i].rpc == nil){
				sendp(work[i].c, m);
				break;
			}
		}
		if (i == Nthreads){
			dprint(2, "%s: all workers busy\n", argv0);
			sendp(work[rr].c, m);
			rr = (rr + 1) % Nthreads;
		}
	}
	threadexits(nil);
}

static void
timerproc(void*)
{
	ulong	now;

	threadsetname("timerproc");
	for(;;){
		sleep(Tmout*1000);
		now = time(nil);
		nbsendul(tmrc, now);	// time does not block!
	}
}

static void
srvinproc(void *)
{
	Frpc*	m;

	threadsetname("srvinproc");
	for(;;){
		m = rpcalloc();
		if (getfcall(srvfd, m) <= 0){
			rpcfree(m);
			fprint(2, "%s: getfcall: %r\n", argv0);
			break;
		}
		sendp(reqc, m);
	}
	sysfatal("no input process");
}

static char*
mkadsaddr(void)
{
	char*	r;
	char*	s;

	s = getenv("fileserver");
	if (s == nil)
		return smprint("tcp!193.147.71.86!11010");
	r =  smprint("tcp!%s!11010", s);
	free(s);
	return r;
}

/* To prevent deadlocks, bns runs with just this: 
 * /mnt/factotum /net /dev /env and /srv
 */

static void
cleanns(void)
{
	int	fd;

	if (bind("#/", "/", MREPL) < 0)
		fprint(2, "FAIL> bind /: %r\n");
	fd = open("#s/factotum", ORDWR);
	mount(fd, -1, "/mnt", MREPL|MCREATE, "");
	close(fd);
	if (bind("#I","/net",  MAFTER) < 0)
		fprint(2, "cannot bind /net: %r\n");
	if (access("/net/tcp/clone", AEXIST) < 0)
		fprint(2, "NO TCP\n");
	if (bind("#c","/dev",  MREPL|MCREATE) < 0)
		fprint(2, "cannot bind /dev: %r\n");
	if (bind("#s","/srv",  MREPL|MCREATE) < 0)
		fprint(2, "cannot bind /srv: %r\n");
	if (bind("#r","/dev",  MAFTER|MCREATE) < 0)
		fprint(2, "cannot bind #r: %r\n");
	bind("#e","/env",  MREPL|MCREATE);
}

static void
mainproc(void* a)
{
	int	fd[2];
	int	cfd[2];
	int	i;
	Channel*c = a;

	threadsetname("mainproc");
	cleanns();
	config(nil);
	if (pipe(fd)<0 || pipe(cfd) < 0)
		sysfatal("pipe: %r");
	if (postfd(sname, fd[0]) < 0)
		sysfatal("can't post srv: %s: %r", sname);
	csname = smprint("%sctl", sname);
	if (postfd(csname, cfd[0]) < 0)
		sysfatal("can't post srv: %s: %r", csname);
	rfork(RFFDG);
	close(fd[0]);
	close(cfd[0]);
	srvfd = fd[1];
	ctlfd = cfd[1];
	reqc = chancreate(sizeof(Frpc*), 0);
	for (i = 0; i < Nthreads; i++)
		work[i].c = chancreate(sizeof(Frpc*), 0);
	repc = chancreate(sizeof(Frpc*), 5);
	tmrc = chancreate(sizeof(ulong), 0);
	assert(reqc && repc);
	if (proccreate(ctlproc, nil, Stack) < 0)
		sysfatal("procrfork: %r");
	if (proccreate(timerproc, nil, Stack) < 0)
		sysfatal("procrfork: %r");
	if (proccreate(srvinproc, nil, Stack) < 0)
		sysfatal("procrfork: %r");
	if (proccreate(dispatchproc, nil, Stack) < 0)
		sysfatal("procrfork: %r");
	free(csname);
	sendul(c, 0);
	threadexits(nil);
}


static void
usage(void)
{
	fprint(2,"usage: %s [-DHdv] [-l] [-s srv]\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	Channel*	sc;
	int		domnt;
	extern int 	newnsdebug;

	sname = "vol";
	adsaddr = nil;
	domnt = 1;
	ARGBEGIN {
	case 'H':
		hdebug++;
		break;
	case 'D':
	case 'd':
		debug++;
		break;
	case 'v':
		vdebug++;
		break;
	case 'l':
		dodiscover++;
		break;
	case 's':
		domnt = 0;
		sname = EARGF(usage());
		break;
	default:
		usage();
	} ARGEND;

	threadsetname("main");
	if (debug > 2)
		mainmem->flags |= POOL_PARANOIA;
	if (argc > 0)
		usage();
	adsaddr = mkadsaddr();
	quotefmtinstall();
	fmtinstall('D', dirfmt);
	fmtinstall('M', dirmodefmt);
	fmtinstall('F', fcallfmt);
	fmtinstall('V', Vfmt);
	fmtinstall('W', Wfmt);
	fmtinstall('X', fidfmt);
	fmtinstall('I', eipfmt);
	fmtinstall('N',	namefmt);
	fmtinstall('K', Kfmt);
	fmtinstall('k', kfmt);
	exprinit();
	sc = chancreate(sizeof(ulong), 0);
	procrfork(mainproc, sc, Stack, RFNOTEG|RFCNAMEG|RFENVG);
	recvul(sc);
	chanfree(sc);
	if (debug>1 || vdebug>1)
		newnsdebug = 1;
	if (domnt){
		newns(getuser(), "/lib/namespace.planb");
		print("starting brc\n");
		procexecl(nil, "/rc/bin/brc", "brc", nil);
		print("brc: %r\nstarting rc\n");
		procexecl(nil, "/bin/rc", "rc", "-i", nil);
	}
	threadexits(nil);
}
