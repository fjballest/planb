#include <u.h>
#include <libc.h>
#include <thread.h>
#include <omero.h>
#include <error.h>
#include <b.h>
#include <plumb.h>
#include <ctype.h>
#include "ox.h"

static int	plumbeditfd = -1;
static int	plumbexecfd = -1;
static Channel*	plumbeditc;
static Channel*	plumbexecc;


static int
addr2ln(Edit* e, char* addr)
{
	char*	fn;
	char*	cmd;
	char	buf[60];
	char*	s;
	Xcmd*	x;

	if (!addr[0] || !e->gtext || !e->gtext->repl)
		return 0;
	fn = smprint("%s/data", e->gtext->repl->path);
	cmd = smprint("grep -n '%s'", addr);
	x = allocxcmd(nil, "/", cmd, nil);
	x->infd[0] = open(fn, OREAD);
	pipe(x->outfd);
	x->pidc = chancreate(sizeof(ulong), 0);
	procrfork(xcmdproc, x, 16*1024, RFFDG|RFNOTEG);
	x->pid = recvul(x->pidc);
	chanfree(x->pidc);
	free(cmd);
	free(fn);
	close(x->infd[0]);
	close(x->outfd[1]);
	memset(buf, 0, sizeof(buf));
	read(x->outfd[0], buf, sizeof(buf));
	close(x->outfd[0]);
	s = strchr(buf ,':');
	if (s)
		*s = 0;
	freexcmd(x);
	if (buf[0] && isdigit(buf[0]))
		return atoi(buf);
	else
		return 0;
}

static void
plumbfile(Plumbmsg* m)
{
	char*	a;
	Edit*	e;
	char*	k;
	char*	path;

	path = cleanpath(m->data, m->wdir);
	e = editfile(path, 0);
	free(path);
	if (e == nil)
		return;
	openpanelctl(e->gcol);
	panelctl(e->gcol, "show\nnomin\n");
	closepanelctl(e->gcol);
	a = plumblookup(m->attr, "addr");
	if (a){
		if (a[0] == '/')
			k = smprint(":%d", addr2ln(e, a+1));
		else if (a[0])
			k = smprint(":%s", a);
		else
			k = nil;
		if (k)
			look(e, k, nil);
		free(k);
	}
}

static void
plumbdata(Plumbmsg* m)
{
	char*	fn;
	Edit*	e;
	char*	t;

	fn = plumblookup(m->attr, "filename");
	if (fn == nil){
		fn = estrdup("Plumb");
	}
	fn = cleanpath(fn, m->wdir);
	t = smprint("[%s]", fn);
	e = musthavemsgs(t);
	free(fn);
	free(t);
	free(e->text);
	e->text = emalloc(m->ndata + 1);
	e->text[m->ndata] = 0;
	memmove(e->text, m->data, m->ndata);
	updatetext(e);
}

static void
plumbeditthread(void*)
{
	Plumbmsg* m;
	char*	a;
	for(;;){
		m = recvp(plumbeditc);
		if (strcmp(m->type, "text")){
			plumbfree(m);
			continue;
		}
		a = plumblookup(m->attr, "action");
		if (a == nil || !strcmp(a, "showfile"))
			plumbfile(m);
		else
			plumbdata(m);
		plumbfree(m);
	}
}


static void
plumbexecthread(void*)
{
	Plumbmsg* m;
	int	i;
	Edit*	e;
	char*	path;

	for(;;){
		m = recvp(plumbexecc);
		if (strcmp(m->type, "text")){
			plumbfree(m);
			continue;
		}
		if (!strncmp(m->data, "exec ", 5))
			if (strchr(":|<>", m->data[5])){
				path = readfstr("/dev/sel");
				e = nil;
				if (path){
					for (i = 0; i < nedits; i++)
						if (edits[i] && strstr(path, edits[i]->gcolname)){
							e = edits[i];
							break;
						}
					if (e)
						run(e, m->data+5, 1, path);
					else
						externrunevent(path, "exec", m->data+5);
					free(path);
				}
			} else {
				xcmd(nil, m->wdir, m->data+5, nil, nil, nil);
			}
		plumbfree(m);
	}
}

void
plumbinit(void)
{
	plumbeditc = createportproc("edit");
	if (plumbeditc == nil)
		fprint(2,"%s: can't create edit port: %r\n", argv0);
	else
		threadcreate(plumbeditthread, 0, 8*1024);
	plumbexecc = createportproc("exec");
	if (plumbexecc == nil)
		fprint(2,"%s: can't create exec port: %r\n", argv0);
	else
		threadcreate(plumbexecthread, 0, 8*1024);
}

