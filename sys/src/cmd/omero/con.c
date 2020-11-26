#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <draw.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <9p.h>
#include "gui.h"


int	condebug;

static Con**	cons;
static int	ncons;

static void
condump(void)
{
	int	i;

	if (!condebug)
		return;
	for (i = 0; i < ncons; i++){
		if (cons[i])
			fprint(2, "con #%d ref %ld addr %s fd %d\n", i,
				cons[i]->ref, cons[i]->addr, cons[i]->fd);
	}
}


static void
conhupproc(void* a)
{
	Con*	c;
	int	x;

	c = a;
	threadsetname("conhupproc");
	read(c->fd, &x, sizeof(x));
	sendp(panelhupc, c);
	threadexits(nil);
}

Con*
newcon(char* addr)
{
	int	i;
	int	cfd;

	condump();
	for (i = 0; i < ncons; i++){
		if (cons[i] == nil)
			break;
		if (!strcmp(cons[i]->addr, addr))
			break;
	}
	if (i == ncons){
		if ((ncons%16) == 0)
			cons = realloc(cons, (ncons+16)*sizeof(Con));
		i = ncons++;
		cons[i] = nil;
	}
	if (cons[i]){
		incref(cons[i]);
		cdprint("newcon: oldcon %d\n", i);
		return(cons[i]);
	}
	edprint("newcon %d %s\n", i, addr);
	cons[i] = emalloc9p(sizeof(Con));
	memset(cons[i], 0, sizeof(Con));
	cons[i]->addr = estrdup9p(addr);
	cons[i]->ref = 1;
	cfd = -1;
	cons[i]->fd = dial(cons[i]->addr, 0, 0, &cfd);
	fprint(cfd, "keepalive 3000");
	close(cfd);
	proccreate(conhupproc, cons[i], 4*1024);
	cdprint("newcon: newcon %d\n", i);
	return cons[i];
}

void
closecon(Con* c)
{
	int	i;

	if (c == nil || decref(c))
		return;
	for (i = 0; i < ncons; i++)
		if (cons[i] == c){
			cons[i] = nil;
			break;
		}
	assert(i < ncons);
	if (c->fd >= 0){
		fprint(c->fd, "bye\001");
		close(c->fd);
	}
	condump();
	cdprint("closecon %d (fd %d)\n", i, c->fd);
	c->fd = -1;
	free(c->addr);
	free(c);
}

int
conprint(Con* c, char* fmt, ...)
{
	va_list	arg;
	int	r;

	// BUG: timeout?
	if (c == nil || c->fd < 0)
		return -1;
	va_start(arg, fmt);
	r = vfprint(c->fd, fmt, arg);
	va_end(arg);
	if (r < 0){
		edprint("conprint: %s: %r\n", c->addr);
		edprint("closing fd %d\n", c->fd);
		close(c->fd);
		c->fd = -1;
	}
	return r;
}
