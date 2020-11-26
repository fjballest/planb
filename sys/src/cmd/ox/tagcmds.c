#include <u.h>
#include <libc.h>
#include <thread.h>
#include <omero.h>
#include <error.h>
#include <b.h>
#include <regexp.h>
#include "ox.h"

enum {
	Nexps = 128
};

typedef struct Fexp Fexp;

struct Fexp {
	char*	exp;
	char*	cmds;
	Reprog*	prog;
};


static char defaultcfg[] =
	"plumb	.*\\.(jpg|JPG|jpeg|gif|GIF|pdf|PDF|ps|PS|mp3)$\n"	
	".*\\.[ch]$	|t+ mk\n"
	".*\\.ms$	| View\n"
	".*\\.pic$	| View\n"
	".*\\.g$	| View\n"
	".*/mkfile$	| mk\n"
	"^\\[/mail/box/[^/]+/mails\\]	| Mails Reply\n"
	"^/mail/box/[^/]+/[^/]+/[0-9]+/(a\.)?[0-9]+/.*text$	| Arch Reply .\n"
	"^/sys/man/[0-9]+/[a-z0-9]+	| View\n";

Fexp*	fexps[Nexps];
int	nfexps;
Reprog*	plumbprog;

void
regerror(char* msg)
{
	fprint(2, "%s: bad tag regexp: %s\n", argv0, msg);
}

static void
compile(char* cfg)
{
	char*	lns[Nexps];
	int	nlns;
	int	i;
	char*	sep;

	nlns = gettokens(cfg, lns, nelem(lns), "\n");

	for (i = 0; i < nlns; i++){
		if (strchr("#\n \t", *lns[i]))
			continue;
		if (nfexps >= nelem(fexps)){
			fprint(2, "%s: too many tag expressions\n", argv0);
			return;
		}
		sep = strrchr(lns[i], '\t');
		if (sep)
			*sep++ = 0;
		if (!strcmp(lns[i], "plumb") && sep != nil){
			plumbprog = regcomp(sep);
			continue;
		}
		fexps[nfexps] = emalloc(sizeof(Fexp));
		fexps[nfexps]->exp = lns[i];
		fexps[nfexps]->cmds = sep;
		if (sep == nil){
			fprint(2, "%s: bad tag entry: %s\n", argv0, lns[i]);
			free(fexps[nfexps]);
			continue;
		}
		fexps[nfexps]->prog = regcomp(fexps[nfexps]->exp);
		if (!fexps[nfexps]->prog){
			fprint(2, "%s: bad regexp: %s\n", argv0, fexps[nfexps]->exp);
			free(fexps[nfexps]);
			continue;
		}
		nfexps++;
	}
}

void
inittagcmds(void)
{
	static char*	cfg;
	char*	fn;
	char*	home;

	home = getenv("home");
	fn = smprint("%s/lib/oxcmds", home);
	cfg = readfstr(fn);
	if (cfg == nil)
		cfg = defaultcfg;
	free(fn);
	compile(cfg);
	free(home);
}

void
tagcmds(char* fn, char* buf, int max)
{
	int	i;

	buf[0] = 0;
	for(i = 0; i < nfexps; i++)
		if(regexec(fexps[i]->prog, fn, nil, 0)){
			strecpy(buf, buf+max, fexps[i]->cmds);
			break;
		}
}

int
mustplumb(char* fn)
{
	return (plumbprog && regexec(plumbprog, fn, nil, 0));
}
