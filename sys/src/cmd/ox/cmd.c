#include <u.h>
#include <libc.h>
#include <thread.h>
#include <omero.h>
#include <error.h>
#include <b.h>
#include "ox.h"

static Xcmd**	xcmds;
static int	nxcmds;
static Channel*	cmdc;

Xcmd*
allocxcmd(char* name, char* dir, char* cmd, char* ox)
{
	Xcmd*	x;

	x = emalloc(sizeof(Xcmd));
	memset(x, 0, sizeof(Xcmd));
	if (name == nil)
		name = "/dev/null";
	x->name= estrdup(name);
	x->dir = estrdup(dir);
	x->cmd = estrdup(cmd);
	if (ox)
		x->ox = estrdup(ox);
	x->infd[0]= x->outfd[1] = -1;
	x->infd[1]= x->outfd[0] = -1;
	x->errfd = -1;
	x->when = time(nil);
	return x;
}

static Xcmd*
newxcmd(char* name, char* dir, char* cmd, char* ox)
{
	Xcmd*	x;

	x = allocxcmd(name, dir, cmd, ox);
	if ((nxcmds%16) == 0)
		xcmds = erealloc(xcmds, (nxcmds+16)*sizeof(Xcmd*));
	xcmds[nxcmds++] = x;
	return x;
}

void
freexcmd(Xcmd* c)
{
	if (c){
		free(c->name);
		free(c->dir);
		free(c->cmd);
		free(c->tag);
		free(c->ox);
		free(c);
	}
}


void
xcmdproc(void* a)
{
	Xcmd*	x;
	int	i;

	x = a;
	if (x->outfd[0] >= 0)
		close(x->outfd[0]);
	if (x->infd[1] >= 0)
		close(x->infd[1]);
	dup(x->outfd[1], 1);
	if (x->errfd >= 0){
		dup(x->errfd, 2);
		close(x->errfd);
	} else
		dup(x->outfd[1], 2);
	close(x->outfd[1]);
	dup(x->infd[0], 0);
	close(x->infd[0]);
	chdir(x->dir);
	for (i = 3; i < 128; i++)
		close(i);
	if (x->name == nil)
		putenv("file", "/dev/null");
	else if (x->name[0] == '[')
		putenv("file", x->dir);
	else
		putenv("file", x->name);
	if (x->ox != nil)
		putenv("panel", x->ox);
	/* Ugly, but convenient to avoid '' around its arguments
	 */
	if (!strncmp(x->cmd, "Clean ", 5))
		procexecl(x->pidc, "/bin/Clean", "Clean", x->cmd+5, nil);

	procexecl(x->pidc, "/bin/rc", "rc", "-c", x->cmd, nil);
	threadexits(nil);
}

static void
gone(int pid, int ok)
{
	int	i;
	char	vmsg[50];
	char*	s;

	for (i = 0; i < nxcmds; i++)
		if (xcmds[i] && xcmds[i]->pid == pid){
			dprint("ox: gone: %s\n", xcmds[i]->cmd);
			if (!ok || (time(nil) - xcmds[i]->when > 2)) {
				s = strchr(xcmds[i]->cmd, ' ');
				if (s)
					*s = 0;
				seprint(vmsg, vmsg+sizeof(vmsg),
					"%s %s\n", xcmds[i]->cmd,
					ok ? "completed" : "failed");
				writefstr("/devs/voice/output", vmsg);
			}
			freexcmd(xcmds[i]);
			xcmds[i] = nil;
			break;
		}
}

static void
waitthread(void*)
{
	Channel*	c;
	Waitmsg*	m;

	threadsetname("waitthread");

	c = threadwaitchan();
	for(;;){
		m = recvp(c);
		gone(m->pid, (m->msg == nil || m->msg[0] == 0));
		free(m);
	}
}

static long
readbuf(Ioproc* io, int fd, char* buf, int len)
{
	int	n;
	int	nr;
	Dir*	d;
	int	more;

	n = 0;
	do {
		nr = ioread(io, fd, buf+n, len - n);
		if (nr <= 0)
			break;
		n += nr;
		d = dirfstat(fd);
		if (d){
			more = d->length;
			free(d);
		} else
			more = 0;
	} while (more && n < len);

	return n;
}

static void
xcmdoutthread(void* a)
{
	Xcmd*	x = a;
	char*	buf;
	int	fd;
	char*	s;
	Edit*	e;
	int	nr,nw;
	int	pid;
	Ioproc*	io;
	long	tot;
	int	i;

	fd = x->outfd[0];
	s = estrdup(x->tag);
	assert(s[0] == '[');
	pid = x->pid;
	buf = emalloc(64*1024+1);
	e = nil;
	io = ioproc();
	assert(io);
	tot = nw = 0;
	for(;;){
		if (e != nil && e->gtext == nil){
			e->pid = 0;
			deledit(e);
			e = nil;
			break;
		}
		nr = readbuf(io, fd, buf, 64*1024);
		if (nr <= 0)
			break;
		if (e == nil){
			e = musthavemsgs(s);
			assert(e);
			assert(e->pid == 0);
			e->pid = pid;
			free(e->text);
			e->text = nil;
			if(openpanel(e->gtext, OWRITE|OTRUNC) < 0){
				msgprint(nil, "%s: %r\n", e->name);
				e->pid = 0;
				break;
			}
		}
		if (tot > 640 * 1024){
			// BUG: If there is too much text loaded, truncate and
			// restart again. Should preserve the last N bytes,
			// but this is enough by now.
			if (e->gtext){
				closepanel(e->gtext);
				openpanel(e->gtext, OWRITE|OTRUNC);
				tot = 0;
			}
		}
		for (i = 0; i < nr; i++)
			if (buf[i] == 0)
				buf[i] = '?'; // precaution
		assert( ( ((ulong)e->gtext)&1 ) == 0);	// BUG hunting
		if (e->gtext != nil)
		if((nw = writepanel(e->gtext, buf, nr)) <= 0){
			msgprint(nil, "%s: %r\n", e->name);
			break;
		} // we ignore other cases due to binary output...
		tot += nw;

	}
	closeioproc(io);
	if (e != nil){
		e->pid = 0;
		if (e->gtext)
			closepanel(e->gtext);
	}
	free(s);
	free(buf);
	close(fd);
	threadexits(nil);
}

void
xcmd(char* name, char* dir, char* arg, char* in, char* out, char* ox)
{
	Xcmd*	x;
	char	cmd[40];
	char*	p;

	x = newxcmd(name, dir, arg, ox);

	if (in)
		x->infd[0] = open(in, OREAD);
	if (0 && x->infd[0] < 0)
		pipe(x->infd);
	if (x->infd[0] < 0)
		x->infd[0] = open("/dev/null", OREAD);

	if (out)
		x->outfd[1] = open(out, OREAD);
	if (x->outfd[1] < 0)
		pipe(x->outfd);
	if (x->outfd[1] < 0)
		x->outfd[1] = open("/dev/null", OWRITE);

	x->pidc = chancreate(sizeof(ulong), 0);
	procrfork(xcmdproc, x, 16*1024, RFFDG|RFNOTEG|RFENVG);
	x->pid = recvul(x->pidc);
	strecpy(cmd, cmd+sizeof(cmd), x->cmd);
	p = strchr(cmd, ' ');
	if (p)
		*p = 0;
	x->tag = smprint("[%s %s %d]", x->dir, cmd, x->pid);
	chanfree(x->pidc);
	x->pidc = nil;

	close(x->infd[0]);
	close(x->outfd[1]);
	x->infd[0] = x->outfd[1] = -1;

	if (x->outfd[0] >= 0){
		threadcreate(xcmdoutthread, x, 32*1024);
	}
}

static int
cput(Edit* e, int argc, char *[], int force)
{
	char*	to;
	Dir*	d;
	long	l;

	if (argc != 1){
		msgprint(nil, "ox: Put does not take arguments\n");
		return 0;
	}
	if (e->qid.type&QTDIR)
		return 1;
	to = gettagpath(e);
	if (!strcmp(to, e->name) && e->sts == Stemp)
		return 1;
	d = dirstat(to);
	if (d != nil){
		if (!force)
		if (d->qid.path != e->qid.path){
			msgprint(nil, "%s: file exists\n", to);
			goto fail;
		} else if (d->qid.vers != e->qid.vers){
			msgprint(nil, "%s: file changed by %s\n", e->name, d->muid);
			goto fail;
		}
	} else
		msgprint(nil, "%s: new file\n", to);
	dprint("put %s\n", to);
	free(e->text);
	e->text = readallpanel(e->gtext, &l);
	if (e->text == nil){
		msgprint(nil, "%s: %r\n", e->gtext->name);
		goto fail;
	}
	l = createf(to, e->text, strlen(e->text), 0664);
	dprint("x: %s: put %ld bytes\n", e->name, l);
	if (l < 0){
		msgprint(nil, "%s: %r\n", to);
		openpanelctl(e->gtext);
		panelctl(e->gtext, "dirty");
		closepanelctl(e->gtext);
		goto fail;
	}
	assert(l == strlen(e->text));
	free(e->text);
	e->text = nil;
	free(e->name);
	e->name = cleanpath(to, nil);
	free(e->dir);
	e->dir = filedir(e->name);
	cleanedit(e, nil);
	openpanelctl(e->gtext);
	panelctl(e->gtext, "clean");
	closepanelctl(e->gtext);
	e->sts = Sclean;
	free(d);
	free(to);
	return 1;
fail:
	free(d);
	free(to);
	return 0;
}

int
cdone(Edit* e, int , char* [], int force)
{
	char*	s;

	if (e->sts != Stemp)
	if (!(e->qid.type&QTDIR))
		if (getsts(e->gtext, e) == Sdirty){
			if (!force){
				s = gettagpath(e);
				msgprint(nil, "%s: unsaved changes\n", s);
				free(s);
				return 0;
			}
		}
	if (e->gtext != nil)
		removepanel(e->gtext);
	if (e->gtag != nil)
		removepanel(e->gtag);
	if (e->gcol != nil)
		removepanel(e->gcol);
	e->gtext = nil;
	e->gtag = nil;
	e->gcol = nil;
	if (e->pid == 0){
		deledit(e);
		musthaveedits();
		if (debug)dumpedits();
	}
	return -1;	// !0 to say ok. <0 to say e is gone
}

int
cexit(Edit* , int , char* [], int force)
{
	int	i;

	for(i = 0; i < nedits; i++)
		if(edits[i] && edits[i]->sts == Sdirty && !force){
			msgprint(nil, "dirty buffers\n");
			return 0;
		}
	threadexitsall(nil);
	return 1;
}

static int
cfont(Edit* e, int , char* [], int )
{
	openpanelctl(e->gtext);
	if (e->font == FR){
		e->font = FT;
		panelctl(e->gtext, "font T");
	} else {
		e->font = FR;
		panelctl(e->gtext, "font R");
	}
	closepanelctl(e->gtext);
	return 1;
}

static int
cget(Edit* e, int argc, char* argv[], int force)
{
	char*	from;
	int	r;

	if (argc == 2){
		from =  cleanpath(argv[1], e->dir);
		editfile(from, 0);
		free(from);
		return 1;
	}
	if (e->sts == Stemp)
		return 1;
	if (!force && !(e->qid.type&QTDIR) && e->sts == Sdirty){
		msgprint(nil, "%s: put changes first\n", e->name);
		return 0;
	}
	from = estrdup(e->name);
	dprint("get %s\n", from);
	r = loadfile(e, from);
	if (r){
		updatetag(e, 1);
		updatetext(e);
		openpanelctl(e->gtext);
		panelctl(e->gtext, "clean");
		closepanelctl(e->gtext); 
	}
	return r;
}

static int
ccmds(Edit*, int, char*[], int)
{
	sendul(cmdc, 0);
	return 1;
}

static Cmd cmds[] = {
	{ "Exit",	cexit},
	{ "Done",	cdone},
	{ "Put",	cput },
	{ "P",		cput },
	{ "Get",	cget },
	{ "G",		cget },
	{ "Font",	cfont},
	{ "Cmds",	ccmds},
};

static char*
readout(int fd)
{
	char*	s;
	int	len;
	int	off;
	int	nr;

	s = emalloc(64*1024);
	len = 64*1024;
	s[0] = 0;
	off = 0;
	for(;;){
		nr = read(fd, s+off, len - off - 1);
		if (nr >= 0){
			s[off+nr] = 0;
			off += nr;
		}
		if (nr <= 0)
			break;
		if (nr == len - off - 1){
			s = erealloc(s, len + 64*1024);
			assert(s);
			len += 64*1024;
		}
	}
	return s;
}

void
wctl(char* path, char* ctl)
{
	char*	fn;

	fn = smprint("%s/ctl", path);
	writefstr(fn, ctl);
	free(fn);
}
	
static void
iocmd(char* graph, char* dir, char* cmd)
{
	char	op;
	int	fd[2];
	char	oname[30];
	char*	in;
	char*	out;
	char*	s;

	op = *cmd++;
	switch(op){
	case '>':
		wctl(graph, "cut\npaste\n");
		in = "/dev/snarf";
		out = nil;
		break;
	case '|':
		pipe(fd);
		// BUG: if sel is x x, then use the whole text
		// as input. so | applies to either sel or whole text.
		wctl(graph, "cut");
		in = "/dev/snarf";
		seprint(oname, oname+30, "/fd/%d", fd[1]);
		out = oname;
		break;
	case '<':
		pipe(fd);
		in = "/dev/null";
		seprint(oname, oname+30, "/fd/%d", fd[1]);
		out = oname;
		break;
	default:
		in = out = nil;
		sysfatal("iocmd: bad cmd %c", op);
	}
	xcmd("/dev/null", dir, cmd, in, out, nil);
	if (out != nil){
		close(fd[1]);
		s = readout(fd[0]);
		close(fd[0]);
		writefstr("/dev/snarf", s);
		wctl(graph, "paste");
		free(s);
	}
}

int
editrun(Panel* t, char* dir, char* arg, char* path)
{
	if (arg[0] == 'E' && arg[1] == ' '){
		editcmd(t, arg+2, path);
		return 1;
	}
	if (strchr("|<>", arg[0])){
		iocmd(path, dir, arg);
		return 1;
	}
	return 0;
}

void
run(Edit* e, char* arg, int, char* path)
{
	char*	ec;
	char	s;
	char*	args[10]; 
	int	nargs;
	int	force;
	int	i;
	int	r;
	char*	ox;

	dprint("ox: %s: run %s\n", e->name, arg);
	if (!arg || !*arg)
		return;
	if (editrun(e->gtext, e->dir, arg, path))
		return;

	for(ec = arg; *ec && !strchr(" \t\n", *ec); ec++)
		;
	s = 0;
	if (ec && *ec){
		s = *ec;
		*ec = 0;
	}
	for (i = 0; i < nelem(cmds); i++)
		if (!strcmp(cmds[i].name, arg)){
			if (ec)
				*ec = s;
			nargs = tokenize(arg, args, nelem(args));
			force = 0;
			if (e->lastev && nargs && !strcmp(e->lastev, args[0]))
				force = 1;
			r = cmds[i].f(e, nargs, args, force);
			dprint("ox: %s: %d\n", args[0], r);
			if (r >= 0){
				free(e->lastev);
				if (!r)
					e->lastev = estrdup(args[0]);
				else
					e->lastev = nil;
			}
			return;
		}
	if (ec)
		*ec = s;
	if (e->gcol != nil && e->gcol->repl != nil)
		ox = e->gcol->repl->path;
	else
		ox = nil;
	xcmd(e->name, e->dir, arg, nil, nil, ox);
	
}

static void
xcmdsthread(void*)
{
	Edit*	e;
	int	i;
	Xcmd*	x;

	threadsetname("xcmdsthread");
	for(;;){
		recvul(cmdc);
		e = musthavemsgs("[Cmds]");
		free(e->text);
		e->text = estrdup("");
		for (i = 0; i < nxcmds; i++)
			if (x = xcmds[i])
				msgprint(e, "Del %d # %s\n", x->pid, x->cmd);
		msgprint(e, "\n");
	}
}

void
cmdinit(void)
{
	cmdc = chancreate(sizeof(ulong), 0);

	threadcreate(waitthread, nil, 8*1024);
	threadcreate(xcmdsthread, nil, 8*1024);
}
