#include <u.h>
#include <libc.h>
#include <plumb.h>

int debug;
char* cmd;

static void
msg(Plumbmsg* m)
{
	char*	pg;
	char*	sec;
	char*	p;

	p = strrchr(m->data, ']');
	if (!p)
		return;
	*p = 0;
	sec = strrchr(m->data, '[');
	if (!sec)
		return;
	*sec++ = 0;
	pg = m->data;
	free(cmd);
	cmd = smprint("man -t %s %s | page -w", sec, pg);
	switch(rfork(RFPROC|RFNOWAIT|RFMEM|RFFDG)){
	case 0:
		execl("/bin/rc", "rc", "-c", cmd, nil);
		exits("exec");
	case -1:
		sysfatal("fork: %r");
	default:
		if (debug)
			fprint(2, "plumbing rc -c '%s'\n", cmd);
	}
}

int
createport(char* name)
{
	int	fd;
	char*	fname;

	fname = smprint("/devs/ports/%s", name);
	assert(fname);
	if (access(fname, AREAD) >= 0)
		fd = open(fname, OREAD|OCEXEC);
	else
		fd = create(fname, OREAD|OCEXEC, 0660);
	free(fname);
	return fd;
}

static void
usage(void)
{
		fprint(2, "usage: %s [-p port] \n", argv0);
		sysfatal("usage");
}

void
main(int argc, char* argv[])
{
	Plumbmsg* m;
	int plumbfd;
	char*	port;


	port="man";
	ARGBEGIN{
	case 'd':
		debug++;
		break;
	case 'p':
		port = EARGF(usage());
	default:
		usage();
	}ARGEND;
	if (argc > 0)
		usage();
	plumbfd = createport(port);
	if (plumbfd < 0)
		sysfatal("port %s: %r", port);
	for(;;){
		m = plumbrecv(plumbfd);
		if(m == nil)
			sysfatal("plumbrecv port %s: %r", port);
		msg(m);
		plumbfree(m);
	}
}
