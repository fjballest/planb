#include <u.h>
#include <libc.h>
#include <thread.h>
#include <omero.h>
#include <error.h>
#include <b.h>
#include "ox.h"

/* 
 * Runs sam's ecmd on e.
 */

void
editcmd(Panel* gtext, char* ecmd, char* path)
{
	int	allfile;
	Xcmd*	x;
	int	efd[2];
	char*	text;
	long	l;
	char	out[512];
	char*	tmpf;
	char*	cmd;
	Sts	s;

	memset(&s, 0, sizeof(s));
	getsts(gtext, &s);
	tmpf = smprint("/tmp/ox.sam.%ld", truerand()%100);
	cmd = smprint("sam -d %s", tmpf);
	allfile = (s.ss == s.se);
	if (allfile)
		wctl(path, "sel all\ncut\n");
	else
		wctl(path, "cut\n");
	text = readfstr("/dev/snarf");
	l = strlen(text);
	if (createf(tmpf, text, l, 0600) < 0){
		free(text);
		free(tmpf);
		free(cmd);
		return;
	}
	free(text);
	x = allocxcmd(nil, "/", cmd, nil);
	free(cmd);
	pipe(x->infd);
	x->outfd[1] = open("/dev/null", OWRITE);
	pipe(efd);
	x->errfd = efd[1];
	x->pidc = chancreate(sizeof(ulong), 0);
	procrfork(xcmdproc, x, 16*1024, RFFDG|RFNOTEG);
	x->pid = recvul(x->pidc);
	chanfree(x->pidc);
	close(efd[1]);
	close(x->outfd[1]);
	close(x->infd[0]);
	fprint(x->infd[1], ",\n%s\n", ecmd);
	fprint(x->infd[1], "w\n");
	close(x->infd[1]);
	for(;;){
		l = read(efd[0], out, sizeof(out)-1);
		if (l <= 0)
			break;
		out[l] = 0;
		dprint("sam output: %s\n", out);
	}
	close(efd[0]);
	freexcmd(x);
	text = readfstr(tmpf);
	remove(tmpf);
	free(tmpf);
	writefstr("/dev/snarf", text);
	wctl(path, "paste");

	free(text);
}
