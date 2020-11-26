#include <u.h>
#include <libc.h>
#include <b.h>
#include <thread.h>

typedef struct Arg Arg;

struct Arg {
	int p[2];
	char**argv;
};

enum {
	Nargs = 64
};

void
cmdproc(void* x)
{
	Arg*	a = x;
	char**	argv;

	argv = a->argv;
	close(a->p[0]);
	dup(a->p[1], 1);
	close(a->p[1]);
	procexec(nil, argv[0], argv);
	threadexits(nil);
}

long
tcmdoutput(char* cmd, char* out, long sz)
{
	long	tot, n;
	Arg	a;
	char*	argv[Nargs];
	int	argc;
	char*	s;

	s = strdup(cmd);
	if (s == nil)
		return -1;
	argc = tokenize(s, argv, nelem(argv)-1);
	if (argc < 1){
		free(s);
		return -1;
	}
	argv[argc] = nil;
	if (pipe(a.p) < 0){
		free(s);
		return -1;
	}
	a.argv = argv;
	procrfork(cmdproc, &a, 8*1024, RFFDG|RFENVG);
	close(a.p[1]);
	for(tot = 0; sz - tot > 1 ; tot +=n){
		n = read(a.p[0], out+tot, sz - tot);
		if (n <= 0)
			break;
	}
	free(s);
	close(a.p[0]);
	out[tot] = 0;
	return tot;
}
