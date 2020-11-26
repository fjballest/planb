#include <u.h>
#include <libc.h>
#include <b.h>

enum {
	Nargs = 64
};

long
cmdoutput(char* cmd, char* out, long sz)
{
	char*	argv[Nargs];
	char*	s;
	int	argc;
	int	p[2];
	long	tot, n;

	s = strdup(cmd);
	if (s == nil)
		return -1;
	argc = tokenize(s, argv, nelem(argv)-1);
	if (argc < 1){
		free(s);
		return -1;
	}
	argv[argc] = nil;
	if (pipe(p) < 0){
		free(s);
		return -1;
	}
	switch(rfork(RFPROC|RFFDG|RFENVG|RFNOWAIT)){
	case -1:
		free(s);
		close(p[0]);
		close(p[1]);
		return -1;
	case 0:
		close(p[0]);
		dup(p[1], 1);
		close(p[1]);
		exec(argv[0], argv);
		_exits("exec env/terms failed");
		return -1;
		break;
	default:
		free(s);
		close(p[1]);
		for(tot = 0; sz - tot > 1 ; tot +=n){
			n = read(p[0], out+tot, sz - tot);
			if (n <= 0)
				break;
		}
		close(p[0]);
		out[tot] = 0;
		return tot;
	}
}
