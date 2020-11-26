#include <u.h>
#include <libc.h>

#define NFD 40

ulong	ival;
char*	cmd;
char**	cmdp;
char**	files;
int	nfiles;
int	once;

static void
usage(void)
{
	fprint(2, "usage: %s [-1] [-i ival] cmd file...\n", argv0);
	exits("usage");
}

static void
cmdf(char* prog)
{
	char	file[50];

	switch(fork()){
	case 0:
		cmdp[0] = prog; // kludge: args processing
		seprint(file, file+sizeof(file), "/bin/%s", prog);
		exec(file, cmdp);
		exits("exec failed");
		break;
	case -1:
		sysfatal("cant fork");
		break;
	default:
		waitpid();
	}
}

static void
poll(ulong ival, char* files[], int nfiles, void (*cmdf)(char*), char* arg)
{
	int	fds[NFD];
	int	i;
	Qid	qids[NFD];
	Dir*	d;
	int	changed;

	if (nfiles > nelem(fds))
		sysfatal("bug: poll: overflow");
	for (i = 0; i < nfiles; i++){
		fds[i] = open(files[i], OREAD|OCEXEC);
		if (fds[i] < 0)
			fprint(2, "can't poll %s: %r\n", files[i]);
	}
	for (i = 0; i < nfiles; i++){
		if (fds[i] < 0)
			continue;
		d = dirfstat(fds[i]);
		if (d == nil){
			close(fds[i]);
			fds[i] = -1;
			continue;
		}
		qids[i] = d->qid;
		free(d);
	}
	for(;;){
		sleep(ival * 1000);
		changed = 0;
		for (i = 0; i < nfiles; i++){
			if (fds[i] < 0)
				continue;
			d = dirfstat(fds[i]);
			if (d == nil){
				close(fds[i]);
				fds[i] = -1;
				continue;
			}
			if (d->qid.path != qids[i].path ||
			    d->qid.vers != qids[i].vers){
				qids[i] = d->qid;
				changed++;
			}
			free(d);
		}
		if (changed){
			cmdf(arg);
			if (once)
				exits(nil);
		}
	}
		
}

void
main(int argc, char* argv[])
{
	ival = 1;
	ARGBEGIN{
	case 'i':
		ival = atoi(EARGF(usage()));
		break;
	case '1':
		once++;
		break;
	default:
		usage();
	}ARGEND;
	if (argc < 2)
		usage();
	if (ival < 1)
		usage();
	cmd = argv[0];
	cmdp= argv;	// kludge
	argv++; argc--;
	files = argv;
	nfiles= argc;

	poll(ival, files, nfiles, cmdf, cmd);
}
