#include <u.h>
#include <libc.h>
#include <thread.h>
#include <bio.h>
#include <b.h>
enum {
	Nclients = 256,
};

typedef struct Cli Cli;

struct Cli {
	ulong	epoch;
	int	fd;
	char*	ad;
	char*	addr;
	int	adfd;
};

int	reportall = 1;
ulong	epoch;
char*	adsdir	= "/lib/ndb/vol";
char*	cvols;
QLock	clilk;
Cli*	cli[Nclients];

#define dprint if(debug)fprint
#define Dprint if(debug>1)fprint

int	debug;

Cli*
newclient(int fd)
{
	int	i;

	qlock(&clilk);
	for (i = 0; i < nelem(cli); i++)
		if (cli[i] == nil)
			break;
	if (i == nelem(cli)){
		qunlock(&clilk);
		fprint(2, "adsrv: fixme: cli table exhausted\n");
		return nil;
	}
	cli[i] = malloc(sizeof(Cli));
	assert(cli[i]);
	cli[i]->addr = nil;
	cli[i]->epoch = 0;
	cli[i]->fd = fd;
	cli[i]->adfd= -1;
	cli[i]->ad = nil;
	qunlock(&clilk);
	return cli[i];
}

void
delclient(Cli* c)
{
	int	i;

	close(c->fd);
	close(c->adfd);	// ORCLOSE
	qlock(&clilk);
	for (i = 0; i < nelem(cli); i++)
		if (cli[i] == c){
			if (cli[i]->ad != nil)
				dprint(2, "gone: %s\n", cli[i]->ad);
			free(cli[i]->ad);
			free(cli[i]);
			free(cli[i]->addr);
			cli[i] = nil;
			break;
		}
	qunlock(&clilk);
}

void
writeannounces(int fd, Cli* c)
{
	int	i;

	qlock(&clilk);
	for (i = 0; i < nelem(cli); i++){
		if (cli[i] != nil && cli[i]->ad != nil)
			if (reportall || cli[i]->epoch > c->epoch)
				fprint(fd, "%s\n", cli[i]->ad);
	}
	if (cvols != nil)
		fprint(fd, "%s", cvols);
	c->epoch = epoch;
	write(fd, "\n", 1);
	qunlock(&clilk);
}

char*
readannounce(Biobuf* bin, Cli* c)
{
	char*	a;
	long	l;
	char*	p;
	char*	s;
	char*	fname;
	char*	path;

	if (c->ad != nil && c->adfd != -1)
		return nil;
	fname = nil;
	a = Brdline(bin, '\n');
	l = Blinelen(bin);
	if (l < 1)
		return nil;
	a[--l] = 0;
	if (l > 0 && a[l-1] == '\r')
		a[--l] = 0;
	if (strncmp(a, "tcp!", 4))
		goto fail;
	s = strchr(a, '\t');
	if (s == nil)
		goto fail;
	*s++ = 0;
	fname = strdup(a);
	p = strchr(fname, '!');
	if (p == nil)
		goto fail;
	p++;
	p = strchr(p, '!');
	if (p == nil)
		goto fail;
	p++;
	path = smprint("%s/%s", adsdir, fname);
	qlock(&clilk);
	c->epoch = ++epoch;
	free(c->ad);
	c->ad = smprint("tcp!%s!%s\t%s", c->addr, p, s);
	if (c->adfd != -1)
		close(c->adfd);
	c->adfd = create(path, OWRITE|ORCLOSE, 0666);
	if (c->adfd == -1 || fprint(c->adfd, "%s\n", c->ad) < 0){
		fprint(2, "%s: %s: %r\n", argv0, s);
		free(c->ad);
		c->ad = nil;
		close(c->adfd);
		c->adfd = -1;
	}
	// leave it open.
	qunlock(&clilk);
	free(path);
	free(fname);
	return c->ad;
fail:
	fprint(2, "bad announce: %s\n", a);
	free(fname);
	return nil;
}

void
clientproc(void* a)
{
	Cli*	c = a;
	Biobuf	bin;
	char*	ln;
	long	l;

	Binit(&bin, c->fd, OREAD);
	while(ln = Brdline(&bin, '\n')){
		l = Blinelen(&bin);
		if (l < 2)
			continue;
		ln[--l] = 0;
		if (ln[l-1] == '\r')
			ln[--l] = 0;
		if (!strcmp(ln, "PlanB announces")){
			Dprint(2, "write announces to %d\n", c->fd);
			writeannounces(c->fd, c);
		} else if (!strcmp(ln, "PlanB announce:")){
			if (readannounce(&bin, c))
				dprint(2, "announce from %d: %s\n", c->fd, c->ad);
		} else
			Dprint(2, "unknown messsage: %s\n", ln);
	}
	Dprint(2, "gone client %d: %r\n", c->fd);
	Bterm(&bin);
	delclient(c);
	threadexits(nil);
}

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
			sys = strdup(buf);
		}
		close(fd);
	}
	if(sys == nil)
		sys = strdup("unknown");
	return sys;
}

void
server(char* addr)
{
	char	adir[40], ldir[40];
	int	cfd, fd;
	Cli*	c;

	cfd = announce(addr, adir);
	if (cfd < 0)
		sysfatal("%s: announce: %r", argv0);
	for (;;){
		cfd = listen(adir, ldir);
		if (cfd < 0)
			sysfatal("%s: listen: %r", argv0);
		fd = accept(cfd, ldir);
		if(fd < 0){
			fprint(2, "%s: accept: %r\n", argv0);
			continue; 
		}
		fprint(cfd, "keepalive 2000\n");
		close(cfd);
		c = newclient(fd);
		c->addr = getremotesys(ldir);
		Dprint(2, "new client %d %s\n", fd, c->addr);
		proccreate(clientproc, c, 16*1024);
	}
	sysfatal("server died");
}

void
usage(void)
{
	fprint(2, "usage: %s [-d] [-n addr] [-c volcfg] [dir]\n", argv0);
	sysfatal("usage");
}

void
threadmain(int argc, char *argv[])
{
	char*	addr;
	char*	cfg;

	addr= "tcp!*!11010";
	cfg = "/lib/ndb/vol/vols";
	ARGBEGIN{
	case 'c':
		cfg = EARGF(usage());
		break;
	case 'n':
		addr = EARGF(usage());
		break;
	case 'd':
		debug++;
		break;
	default:
		usage();
	}ARGEND;
	switch(argc){
	case 0:
		break;
	case 1:
		adsdir = argv[0];
		break;
	default:
		usage();
	}
	if (cfg != nil && access(cfg, AREAD) == 0)
		cvols = readfstr(cfg);
	dprint(2, "%s %s %s\n", argv0, adsdir, addr);
	server(addr);
	threadexits(nil);
}

