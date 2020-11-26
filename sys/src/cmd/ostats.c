#include <u.h>
#include <libc.h>
#include <thread.h>
#include <ctype.h>
#include <auth.h>
#include <fcall.h>
#include <omero.h>
#include <error.h>
#include <b.h>

typedef struct Plot	Plot;
typedef struct Machine	Machine;

enum {
	Mbattery,
	Mcontext,
	Mether,
	Methererr,
	Metherin,
	Metherout,
	Mfault,
	Midle,
	Minintr,
	Mintr,
	Mload,
	Mmem,
	Mswap,
	Msyscall,
	Mtlbmiss,
	Mtlbpurge,
	Msignal,
	Mend,
};

enum
{
	/* /dev/swap */
	Mem		= 0,
	Maxmem,
	Swap,
	Maxswap,
	/* /dev/sysstats */
	Procno	= 0,
	Context,
	Interrupt,
	Syscall,
	Fault,
	TLBfault,
	TLBpurge,
	Load,
	Idle,
	InIntr,
	/* /net/ether0/stats */
	In		= 0,
	Link,
	Out,
	Err0,

	MAXNUM	= 10,
	Dot		= 2,	/* height of dot */
};

struct Plot
{
	int		*data;
	int		ndata;
	char		*label;
	int		overflow;
	void		(*newvalue)(Machine*, ulong*, ulong*, int);
	Panel*		graph;
};


struct Machine
{
	char		*name;
	int		remote;
	int		statsfd;
	int		swapfd;
	int		etherfd;
	int		ifstatsfd;
	int		batteryfd;
	int		bitsybatfd;
	int		disable;

	ulong		devswap[4];
	ulong		devsysstat[10];
	ulong		prevsysstat[10];
	int		nproc;
	ulong		netetherstats[8];
	ulong		prevetherstats[8];
	ulong		batterystats[2];
	ulong		netetherifstats[2];

	char		buf[1024];
	char		*bufp;
	char		*ebufp;
};

char	*mname[Mend+1] = {
	"battery",
	"context",
	"ether",
	"ethererr",
	"etherin",
	"etherout",
	"fault",
	"idle",
	"inintr",
	"intr",
	"load",
	"mem",
	"swap",
	"syscall",
	"tlbmiss",
	"tlbpurge",
	"802.11b",
	nil,
};

int	present[Mend];
Plot	*plot[20];
int	nplot;
int	sleeptime;
char*	machine;
Panel*	grow;
Machine	*m;
int vsize = 40;
int hsize = 100;


void
memval(Machine *m, ulong *v, ulong *vmax, int)
{
	*v = m->devswap[Mem];
	*vmax = m->devswap[Maxmem];
}

void
swapval(Machine *m, ulong *v, ulong *vmax, int)
{
	*v = m->devswap[Swap];
	*vmax = m->devswap[Maxswap];
}

void
contextval(Machine *m, ulong *v, ulong *vmax, int init)
{
	*v = m->devsysstat[Context]-m->prevsysstat[Context];
	*vmax = sleeptime*m->nproc;
	if(init)
		*vmax = sleeptime;
}

void
intrval(Machine *m, ulong *v, ulong *vmax, int init)
{
	*v = m->devsysstat[Interrupt]-m->prevsysstat[Interrupt];
	*vmax = sleeptime*m->nproc;
	if(init)
		*vmax = sleeptime;
}

void
syscallval(Machine *m, ulong *v, ulong *vmax, int init)
{
	*v = m->devsysstat[Syscall]-m->prevsysstat[Syscall];
	*vmax = sleeptime*m->nproc;
	if(init)
		*vmax = sleeptime;
}

void
faultval(Machine *m, ulong *v, ulong *vmax, int init)
{
	*v = m->devsysstat[Fault]-m->prevsysstat[Fault];
	*vmax = sleeptime*m->nproc;
	if(init)
		*vmax = sleeptime;
}

void
tlbmissval(Machine *m, ulong *v, ulong *vmax, int init)
{
	*v = m->devsysstat[TLBfault]-m->prevsysstat[TLBfault];
	*vmax = (sleeptime/1000)*10*m->nproc;
	if(init)
		*vmax = (sleeptime/1000)*10;
}

void
tlbpurgeval(Machine *m, ulong *v, ulong *vmax, int init)
{
	*v = m->devsysstat[TLBpurge]-m->prevsysstat[TLBpurge];
	*vmax = (sleeptime/1000)*10*m->nproc;
	if(init)
		*vmax = (sleeptime/1000)*10;
}

void
loadval(Machine *m, ulong *v, ulong *vmax, int init)
{
	*v = m->devsysstat[Load];
	*vmax = 1000*m->nproc;
	if(init)
		*vmax = 1000;
}

void
idleval(Machine *m, ulong *v, ulong *vmax, int)
{
	*v = m->devsysstat[Idle]/m->nproc;
	*vmax = 100;
}

void
inintrval(Machine *m, ulong *v, ulong *vmax, int)
{
	*v = m->devsysstat[InIntr]/m->nproc;
	*vmax = 100;
}

void
etherval(Machine *m, ulong *v, ulong *vmax, int init)
{
	*v = m->netetherstats[In]-m->prevetherstats[In] + m->netetherstats[Out]-m->prevetherstats[Out];
	*vmax = sleeptime*m->nproc;
	if(init)
		*vmax = sleeptime;
}

void
etherinval(Machine *m, ulong *v, ulong *vmax, int init)
{
	*v = m->netetherstats[In]-m->prevetherstats[In];
	*vmax = sleeptime*m->nproc;
	if(init)
		*vmax = sleeptime;
}

void
etheroutval(Machine *m, ulong *v, ulong *vmax, int init)
{
	*v = m->netetherstats[Out]-m->prevetherstats[Out];
	*vmax = sleeptime*m->nproc;
	if(init)
		*vmax = sleeptime;
}

void
ethererrval(Machine *m, ulong *v, ulong *vmax, int init)
{
	int i;

	*v = 0;
	for(i=Err0; i<nelem(m->netetherstats); i++)
		*v += m->netetherstats[i];
	*vmax = (sleeptime/1000)*10*m->nproc;
	if(init)
		*vmax = (sleeptime/1000)*10;
}

void
batteryval(Machine *m, ulong *v, ulong *vmax, int)
{
	*v = m->batterystats[0];
	if(m->bitsybatfd >= 0)
		*vmax = 184;		// at least on my bitsy...
	else
		*vmax = 100;
}

void
signalval(Machine *m, ulong *v, ulong *vmax, int)
{
	ulong l;

	*vmax = sleeptime;
	l = m->netetherifstats[0];
	/*
	 * Range is seen to be from about -45 (strong) to -95 (weak); rescale
	 */
	if(l == 0){	/* probably not present */
		*v = 0;
		return;
	}
	*v = 20*(l+95);
}

void	(*newvaluefn[Mend])(Machine*, ulong*, ulong*, int init) = {
	batteryval,
	contextval,
	etherval,
	ethererrval,
	etherinval,
	etheroutval,
	faultval,
	idleval,
	inintrval,
	intrval,
	loadval,
	memval,
	swapval,
	syscallval,
	tlbmissval,
	tlbpurgeval,
	signalval,
};

int
needswap(void)
{
	return present[Mmem] | present[Mswap];
}


int
needstat(void)
{
	return  present[Mcontext]  | present[Mfault] | present[Mintr] | present[Mload] | present[Midle] |
		present[Minintr] | present[Msyscall] | present[Mtlbmiss] | present[Mtlbpurge];
}


int
needether(void)
{
	return  present[Mether] | present[Metherin] | present[Metherout] | present[Methererr];
}

int
needbattery(void)
{
	return  present[Mbattery];
}

int
needsignal(void)
{
	return  present[Msignal];
}

int 
connectexportfs(char *addr)
{
	char buf[ERRMAX], dir[256], *na;
	int fd, n;
	char *tree;
	AuthInfo *ai;

	tree = "/";
	na = netmkaddr(addr, 0, "exportfs");
	if((fd = dial(na, 0, dir, 0)) < 0)
		return -1;

	ai = auth_proxy(fd, nil, "proto=p9any role=client");
	if(ai == nil)
		return -1;

	n = write(fd, tree, strlen(tree));
	if(n < 0){
		close(fd);
		return -1;
	}

	strcpy(buf, "can't read tree");
	n = read(fd, buf, sizeof buf - 1);
	if(n!=2 || buf[0]!='O' || buf[1]!='K'){
		buf[sizeof buf - 1] = '\0';
		werrstr("bad remote tree: %s\n", buf);
		close(fd);
		return -1;
	}

	return fd;
}

int
loadbuf(Machine *m, int *fd)
{
	int n;


	if(*fd < 0)
		return 0;
	seek(*fd, 0, 0);
	n = read(*fd, m->buf, sizeof m->buf);
	if(n <= 0){
		close(*fd);
		*fd = -1;
		return 0;
	}
	m->bufp = m->buf;
	m->ebufp = m->buf+n;
	return 1;
}

int
readnums(Machine* m, int n, ulong *a, int spanlines)
{
	int i;
	char *p, *ep;

	if(spanlines)
		ep = m->ebufp;
	else
		for(ep=m->bufp; ep<m->ebufp; ep++)
			if(*ep == '\n')
				break;
	p = m->bufp;
	for(i=0; i<n && p<ep; i++){
		while(p<ep && !isdigit(*p) && *p!='-')
			p++;
		if(p == ep)
			break;
		a[i] = strtoul(p, &p, 10);
	}
	if(ep < m->ebufp)
		ep++;
	m->bufp = ep;
	return i == n;
}

int
initmach(char *name)
{
	int n, fd;
	ulong a[MAXNUM];
	char *p, mpt[256], buf[256];

	p = strchr(name, '!');
	if(p)
		p++;
	else
		p = name;
	m->name = estrdup(p);
	m->remote = (strcmp(p, sysname()) != 0);
	if(m->remote == 0)
		strcpy(mpt, "");
	else{
		snprint(mpt, sizeof mpt, "/n/%s", p);
		fd = connectexportfs(name);
		if(fd < 0){
			fprint(2, "can't connect to %s: %r\n", name);
			return 0;
		}
		/* BUG? need to use amount() now? */
		if(mount(fd, -1, mpt, MREPL, "") < 0)
			return 0;
	}

	seprint(buf, buf+sizeof(buf), "%s/dev/swap", mpt);
	m->swapfd = open(buf, OREAD);
	if(loadbuf(m, &m->swapfd) && readnums(m, nelem(m->devswap), a, 0))
		memmove(m->devswap, a, sizeof m->devswap);
	else
		m->devswap[Maxmem] = m->devswap[Maxswap] = 100;

	snprint(buf, sizeof buf, "%s/dev/sysstat", mpt);
	m->statsfd = open(buf, OREAD);
	if(loadbuf(m, &m->statsfd)){
		for(n=0; readnums(m, nelem(m->devsysstat), a, 0); n++)
			;
		m->nproc = n;
	}else
		m->nproc = 1;

	snprint(buf, sizeof buf, "%s/net/ether0/stats", mpt);
	m->etherfd = open(buf, OREAD);
	if(loadbuf(m, &m->etherfd) && readnums(m, nelem(m->netetherstats), a, 1))
		memmove(m->netetherstats, a, sizeof m->netetherstats);

	snprint(buf, sizeof buf, "%s/net/ether0/ifstats", mpt);
	m->ifstatsfd = open(buf, OREAD);
	if(loadbuf(m, &m->ifstatsfd)){
		/* need to check that this is a wavelan interface */
		if(strncmp(m->buf, "Signal: ", 8) == 0 && readnums(m, nelem(m->netetherifstats), a, 1))
			memmove(m->netetherifstats, a, sizeof m->netetherifstats);
	}

	snprint(buf, sizeof buf, "%s/mnt/apm/battery", mpt);
	m->batteryfd = open(buf, OREAD);
	m->bitsybatfd = -1;
	if(m->batteryfd >= 0){
		if(loadbuf(m, &m->batteryfd) && readnums(m, nelem(m->batterystats), a, 0))
			memmove(m->batterystats, a, sizeof(m->batterystats));
	}else{
		snprint(buf, sizeof buf, "%s/dev/battery", mpt);
		m->bitsybatfd = open(buf, OREAD);
		if(loadbuf(m, &m->bitsybatfd) && readnums(m, 1, a, 0))
			memmove(m->batterystats, a, sizeof(m->batterystats));
	}
	return 1;
}

void
readmach(void)
{
	int n, i;
	ulong a[8];
	char buf[32];

	snprint(buf, sizeof buf, "%s", m->name);
	if (strcmp(m->name, buf) != 0){
		free(m->name);
		m->name = estrdup(buf);
	}
	if(needswap() && loadbuf(m, &m->swapfd) && readnums(m, nelem(m->devswap), a, 0))
		memmove(m->devswap, a, sizeof m->devswap);
	if(needstat() && loadbuf(m, &m->statsfd)){
		memmove(m->prevsysstat, m->devsysstat, sizeof m->devsysstat);
		memset(m->devsysstat, 0, sizeof m->devsysstat);
		for(n=0; n<m->nproc && readnums(m, nelem(m->devsysstat), a, 0); n++)
			for(i=0; i<nelem(m->devsysstat); i++)
				m->devsysstat[i] += a[i];
	}
	if(needether() && loadbuf(m, &m->etherfd) && readnums(m, nelem(m->netetherstats), a, 1)){
		memmove(m->prevetherstats, m->netetherstats, sizeof m->netetherstats);
		memmove(m->netetherstats, a, sizeof m->netetherstats);
	}
	if(needsignal() && loadbuf(m, &m->ifstatsfd) && strncmp(m->buf, "Signal: ", 8)==0 && readnums(m, nelem(m->netetherifstats), a, 1)){
		memmove(m->netetherifstats, a, sizeof m->netetherifstats);
	}
	if(needbattery() && loadbuf(m, &m->batteryfd) && readnums(m, nelem(m->batterystats), a, 0))
		memmove(m->batterystats, a, sizeof(m->batterystats));
	if(needbattery() && loadbuf(m, &m->bitsybatfd) && readnums(m, 1, a, 0))
		memmove(m->batterystats, a, sizeof(m->batterystats));

}

Plot*
newplot(int n)
{
	Plot *g;
	static int nadd;
	Panel*	w;
	Panel*  wl;
	char	txt[20];
	char	gname[20];

	assert(n < nelem(mname));
	/* avoid two adjacent plots of same color */
	g = emalloc(sizeof(Plot));
	memset(g, 0, sizeof(Plot));
	g->label = mname[n];
	g->data = malloc(hsize*sizeof(ulong));
	g->ndata = hsize;
	memset(g->data, 0, hsize*sizeof(ulong));
	seprint(txt, txt+sizeof(txt), "%s %s", machine, mname[n]);
	txt[13]=0;
	seprint(gname, gname+sizeof(gname), "col:%s", mname[n]);
	w = createsubpanel(grow, gname);
	seprint(gname, gname+sizeof(gname), "label:%s", mname[n]);
	wl = createsubpanel(w, gname);
	//graphctl(wl, "font S");
	openpanel(wl, OWRITE|OTRUNC);
	writepanel(wl, txt, strlen(txt));
	closepanel(wl);
	seprint(gname, gname+sizeof(gname), "draw:%s", mname[n]);
	g->graph = createsubpanel(w, gname);
	openpanelctl(w);
	panelctl(w, "notag");
	closepanelctl(w);
	g->newvalue = newvaluefn[n];
	present[n] = 1;
	plot[nplot++] = g;
	return g;
}

void
update(Plot *g, ulong v, ulong vmax)
{
	static char plot[32*1024];
	char*	s;
	double	y;
	int	d;
	int	i;

	memmove(g->data+1, g->data, (g->ndata-1)*sizeof(g->data[0]));
	g->data[0] = v;
	s = seprint(plot, plot+sizeof(plot),
		"rect %d %d %d %d grey\n", 0, 0, hsize+1, vsize+1);
	for (i = 0; i < g->ndata; i++){
		y = ((double)g->data[i])/(vmax*1);
		d = vsize * y;
		if (d < 0)
			d = 0;
		if (d > vsize)
			d = vsize;
		d = vsize - d;
		s = seprint(s, plot+sizeof(plot),
			"line %d %d %d %d 0 red\n", hsize - i, vsize, hsize - i, d);
	}
	openpanel(g->graph, OWRITE|OTRUNC);
	writepanel(g->graph, plot, strlen(plot));
	closepanel(g->graph);
}

void
usage(void)
{
	fprint(2, "usage: %s [-8bceEfiImlnpstw] [machine]\n", argv0);
	exits("usage");
}

void
omerogone(void)
{
	fprint(2, "ostats: graphgone\n");
	sysfatal("graphgone");
}

void
threadmain(int argc, char* argv[])
{
	ulong v, vmax, nargs;
	char* s;
	int	i;
	char args[20];
	char	argchars[] = "8bceEfiImlnpstw";
	extern int graphdebug;

	grow = createpanel("ostats", "row", nil);
	if (grow == nil)
		sysfatal("graphinit: %r\n");
	sleeptime = 2000;
	nargs = 0;
	ARGBEGIN{
	case 'd':
		omerodebug = 1;
		break;
	case 'T':
		s = EARGF(usage());
		i = atoi(s);
		if(i > 2)
			sleeptime = 1000*i;
		break;
	default:
		if(nargs>=sizeof args || strchr(argchars, ARGC())==nil)
			usage();
		args[nargs++] = ARGC();
	}ARGEND;
	if (argc == 1){
		machine = argv[0];
		argc--;
	} else
		machine = sysname();
	if (argc != 0)
		usage();

	m = malloc(sizeof(Machine));
	memset(m, 0, sizeof(Machine));
	initmach(machine);
	for(i=0; i<nargs; i++)
	switch(args[i]){
	case 'b':
		newplot(Mbattery);
		break;
	case 'c':
		newplot(Mcontext);
		break;
	case 'e':
		newplot(Mether);
		break;
	case 'E':
		newplot(Metherin);
		newplot(Metherout);
		break;
	case 'f':
		newplot(Mfault);
		break;
	case 'i':
		newplot(Mintr);
		break;
	case 'I':
		newplot(Mload);
		newplot(Midle);
		newplot(Minintr);
		break;
	case 'l':
		newplot(Mload);
		break;
	case 'm':
		newplot(Mmem);
		break;
	case 'n':
		newplot(Metherin);
		newplot(Metherout);
		newplot(Methererr);
		break;
	case 'p':
		newplot(Mtlbpurge);
		break;
	case 's':
		newplot(Msyscall);
		break;
	case 't':
		newplot(Mtlbmiss);
		newplot(Mtlbpurge);
		break;
	case '8':
		newplot(Msignal);
		break;
	case 'w':
		newplot(Mswap);
		break;
	}
	if(nplot == 0)
		newplot(Mload);
	closepanelctl(grow);
	for (;;){
		readmach();
		for(i=0; i<nplot; i++){
			plot[i]->newvalue(m, &v, &vmax, 0);
			update(plot[i], v, vmax);
		}
		sleep(sleeptime);
	}

}
