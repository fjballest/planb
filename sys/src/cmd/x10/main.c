#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <bio.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include "x10.h"

typedef struct Cmd Cmd;

struct Cmd {
	char*	name;
	int	nargs;
	int	(*f)(X10*, int, char**);
};


int	quiet;
int	interactive;
int	mainstacksize = 32*1024;
char	logf[] = "x10X";

static int	
func(X10*x, int, char** args)
{
	char	hc;
	int	dc;
	int	i;
	int	dim;

	for (i = 0; i < Fmax; i++)
		if (strcmp(args[0], x10fnames[i]) == 0)
			break;
	if (i == Fmax)
		return -1;
	hc=args[1][0];
	dc=1;
	dim = 0;
	if (args[2] != nil)
		dc=atoi(args[2]);
	if (args[3] != nil)
		dim = atoi(args[3]);
	if (x10reqaddr(x, hc, dc) < 0 || x10reqfunc(x, i, dim) < 0)
		return -1;
	return 0;
}

static int
sts(X10* x, int, char**)
{
	return x10reqsts(x);
}

static int
dump(X10*x, int, char**)
{
	x10print(2, x);
	return 0;
}

static int
quit(X10* x, int, char**)
{
	x10close(x);
	threadexitsall(nil);
	return 0;
}

static int
devs(X10* x, int, char**)
{
	static char*	ons[] = { "off", "on " };
	Dev*	d;
	int	i;

	if (x10reqsts(x) < 0)
		return -1;
	d = x10devs(x);
	for(i = 0; i < Ndevs; i++){
		if (d[i].hc != 0)
			fprint(2, "dev %c %d %s dim %d\n",
				hctochr(d[i].hc), dctoint(d[i].dc),
				ons[d[i].on], d[i].dim);
	}
	return 0;
}

static int dbg(X10*, int, char**args)
{
	debug = atoi(args[1]);
	return 0;
}

static int help(X10*, int, char**);

static char	prompt[] = "\n→ ";
static Cmd	cmds[] = {
	// All X10 funcs plus..
	{"s",	0,	sts},
	{"sts", 0,	sts},
	{"d",	0,	dump},
	{"dump",	0,	dump},
	{"q",	0,	quit},
	{"quit",0,	quit},
	{"p",	0,	devs},
	{"print",0,	devs},
	{"?",	0,	help},
	{"D",	1,	dbg},
};

// 0: disabled; 1 hc; 2 hc dc; 3 hc dc dim
static int x10fargs[] = {
	1, 1, 2, 2,
	3, 3, 1, 1,
	1, 1, 2, 2,
	1, 1, 1, 1,
};

static char* fusage[] = {
	"—disabled—",
	"hc",
	"hc dc",
	"hc dc dim",
};

static int
help(X10*, int, char**)
{
	int	i;
	int	a;
	print("\ts status\n");
	print("\td dump\n");
	print("\tq quit\n");
	print("\tp print\n");
	for (i = 0; i < Fmax; i++){
		a = x10fargs[i];
		if (a != 0)
			print("\t%s %s\n", x10fnames[i], fusage[a]);
	}
	return 0;
}

int
runfunc(X10* x, int nargs, char** args)
{
	int	i;
	int	a;

	for (i = 0; i < Fmax; i++)
		if (strcmp(args[0], x10fnames[i]) == 0)
			break;
	if (i == Fmax)
		return 0;
	a = x10fargs[i];
	if (a == 0)
		return 0;
	if (nargs < 1 + a){
		if (interactive)
			print("%s: wrong number of arguments\n", args[0]);
		else
			return -1;
	} else
		if (func(x, nargs, args) < 0){
			if (interactive)
				print("%s: failed\n", args[0]);
			return -1;
		}
	return 1;
}

static int
runcmd(X10* x, int nargs, char** args)
{
	int	i;

	for (i = 0; i < nelem(cmds); i++)
		if (strcmp(args[0], cmds[i].name) == 0)
			break;
	if (i == nelem(cmds))
		return 0;

	if (cmds[i].nargs >= 0 && 1+cmds[i].nargs != nargs)
		print("%s: wrong number of arguments\n", args[0]);
	else
 		if (cmds[i].f(x, nargs, args) < 0)
			print("%s: failed\n", cmds[i].name);
	return 1;
}

static void
sh(X10* x)
{
	Biobuf bin;
	char*	args[10];
	int	nargs;
	char*	ln;

	Binit(&bin, 0, OREAD);
	for(;;){
		if (!quiet)
			print("%s", prompt);
		ln = Brdstr(&bin, '\n', 1);
		if (ln == nil)
			break;
		if (ln[0] == '\n' || ln[0] == 0)
			continue;
		memset(args, 0, sizeof(args));
		nargs = tokenize(ln, args, nelem(args));
		if (nargs == 0)
			continue;
		if (nargs == nelem(args))
			sysfatal("bug: sh: fixme: Nargs overflow");
		if (args[0] == nil){
			print("sh: null command\n");
			continue;
		}
		if (!runfunc(x, nargs, args) && !runcmd(x, nargs, args))
			print("?\n");
	}
}




static void
usage(void)
{
	fprint(2, "usage: %s  [-Diq] [-h hc] [-d dev] [-f cnf]\n", argv0);
	threadexitsall("usage");
}

void
threadmain(int argc, char* argv[])
{
	X10*	x10;
	char*	dev;
	char*	conf;
	char	hc;
	int	net;

	net = 1;
	dev = "/dev/eia0";
	conf= "/sys/lib/x10conf";
	hc = 'a';
	ARGBEGIN{
	case 'i':
		interactive = 1;
		break;
	case 'D':
		if (debug)
			chatty9p++;
		debug = 1;
		break;
	case 'q':
		quiet = 1;
		break;
	case 'd':
		dev = EARGF(usage());
		break;
	case 'f':
		conf= EARGF(usage());
		break;
	case 'h':
		hc = *EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;
	if (argc != 0)
		usage();
	logf[3] = hc;
	x10 = x10open(dev, hc);
	x10reqsts(x10);
	sleep(1000);
	if (net){
		pfs(x10, hc, conf);
	} else if (interactive){
		sh(x10);
		x10close(x10);
	} else
		usage();
	sleep(3*1000);
	threadexits(nil);
}
