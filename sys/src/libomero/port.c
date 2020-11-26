#include <u.h>
#include <libc.h>
#include <thread.h>
#include <bio.h>
#include <omero.h>
#include <auth.h>
#include <plumb.h>

typedef struct Arg Arg;

struct Arg {
	int	fd;
	Channel*c;
};

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
plumbproc(void* a)
{
	Arg*	p;
	Plumbmsg *m;

	threadsetname("plumbproc");
	p = a;
	for(;;){
		m = plumbrecv(p->fd);
		sendp(p->c, m);
		if(m == nil)
			break;
	}
	threadexits(nil);
}

Channel*
createportproc(char* port)
{
	Arg*	a;
	int	fd;

	fd = createport(port);
	if (fd < 0)
		return nil;
	a = malloc(sizeof(Arg));
	assert(a);
	a->fd = fd;
	a->c = chancreate(sizeof(Plumbmsg*), 0);
	proccreate(plumbproc, a, 8*1024);
	return a->c;
}
