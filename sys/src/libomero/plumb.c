#include <u.h>
#include <libc.h>
#include <thread.h>
#include <bio.h>
#include <omero.h>
#include <auth.h>
#include <plumb.h>

static int	plumbsendfd = -1;

void
evhistory(char* prg, char* ev, char* arg)
{
	static char* hfname = nil;
	char*	home;
	int	fd;

	if (hfname == nil){
		home = getenv("home");
		hfname = smprint("%s/ohist", home);
		free(home);
	}
	fd = open(hfname, OWRITE);
	if (fd >= 0){
		seek(fd, 2, 0);
		fprint(fd, "%s %s %s\n", prg, ev, arg);
		close(fd);
	}
}

int
plumblook(char* dir, char* arg)
{
	Plumbmsg*m;
	int	l;
	int	ok;

	if (!arg || !*arg)
		return 0;
	if (plumbsendfd < 0)
		plumbsendfd = plumbopen("send", OWRITE|OCEXEC);
	if (plumbsendfd < 0)
		return 0;
	m = malloc(sizeof(Plumbmsg));
	if (m == nil)
		return 0;
	m->src = strdup(argv0);
	m->dst = nil;
	m->wdir= strdup(dir);
	m->type = strdup("text");
	m->attr = nil;
	m->data = strdup(arg);
	m->ndata= -1;
	assert(m->wdir && m->src && m->data);
	ok = plumbsend(plumbsendfd, m) >= 0;
	if (!ok){
		l = strlen(m->data);
		if (m->data[l-1] == ':'){
			// Might be file:nb: instead of file:nb
			// due to selection expand in omero.
			m->data[l-1] = 0;
			m->ndata= -1;
			ok = plumbsend(plumbsendfd, m) >= 0;
		}
	}
	plumbfree(m);
	return ok;
}

int
plumbexec(char* dir, char* arg)
{
	Plumbmsg*m;

	assert(strlen(arg)>12);
	if (arg)
		arg+= 12;

	if (plumbsendfd < 0)
		plumbsendfd = plumbopen("send", OWRITE|OCEXEC);
	if (plumbsendfd < 0){
		fprint(2, "plumbopen: send: %r\n");
		return 0;
	}
	m = malloc(sizeof(Plumbmsg));
	if (m == nil)
		return 0;
	m->src = strdup(argv0);
	m->dst = strdup("exec");
	m->wdir= strdup(dir);
	m->type = strdup("text");
	m->attr = nil;
	m->data = smprint("exec %s", arg);
	m->ndata= -1;
	assert(m->wdir && m->src && m->data);
	if (plumbsend(plumbsendfd, m) < 0){
		fprint(2, "plumbexec: %r\n");
		plumbfree(m);
		return 0;
	}
	plumbfree(m);
	return 1;
}
