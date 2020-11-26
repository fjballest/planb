#include <u.h>
#include <libc.h>
#include <thread.h>
#include <error.h>
#include <b.h>
#include <omero.h>
#include <keyboard.h>
#include "ox.h"

static char*	homedir;
static Edit*	msgedit;
static char*	where;	// ...is ox creating panels
static int	space;	// omero workspace

Edit**	edits;
int	nedits;
int	debug;

int
omerogone(void)
{
	dprint("omero is going\n");
	postnote(PNGROUP, getpid(), "omerogone");
	threadexitsall("omerogone");
	return 1;
}

static Edit*
findedit(char* p)
{
	int	i;

	for(i = 0; i < nedits; i++)
		if(edits[i] && !strcmp(edits[i]->name, p))
			return edits[i];
	return nil;
}

Edit*
musthavemsgs(char* msgs)
{
	char*	s;
	Edit*	e;

	if(msgs == nil)
		s = msgs = smprint("[%s]", homedir);
	else
		s = nil;
	assert(msgs[0] == '[');
	e = findedit(msgs);
	if(e == nil){
		e = editfile(msgs, 1);
		if(e){
			openpanelctl(e->gtext);
			panelctl(e->gtext, "mark 1");
			closepanelctl(e->gtext);
			if (s != nil)
				msgedit = e;
		} else
			fprint(2, "%s: can't create %s panel\n", argv0, s);
	}
	free(s);
	return e;
}

void
musthaveedits(void)
{
	int	i;

	for(i = 0; i < nedits; i++)
		if(edits[i])
			return;
	// lastedit was gone. terminate.
	postnote(PNGROUP, getpid(), "noedits");
	sysfatal("noedits");
}

static void
dumpedit(Edit* e)
{
	char*	sts = "????";

	switch(e->sts){
	case Sclean:
		sts = "clean";
		break;
	case Sdirty:
		sts = "dirty";
		break;
	case Stemp:
		sts = "temp";
		break;
	}
	fprint(2, "ox: ed %-30s\t(%s, %s t=%011ld)\n\tgcol %s\n",
		e->name, e->dir, sts, e->atime, e->gcolname);
}

void
dumpedits(void)
{
	int	i;

	fprint(2, "panels:\n");
	paneldump(2);
	fprint(2, "edits:\n");
	for(i = 0; i < nedits; i++)
		if(edits[i])
			dumpedit(edits[i]);
	fprint(2, "\n");
}

static void
freeedit(Edit* e)
{
	if(e == msgedit)
		msgedit = nil;
	free(e->name);
	free(e->dir);
	free(e->text);
	free(e->lastev);
	free(e->gcolname);
	memset(e, 3, sizeof(*e));	// poison
	free(e);
}

void
msgprint(Edit* e, char* fmt, ...)
{
	static int nested;
	va_list arg;
	char*	msg;
	char*	txt;


	if(nested){
		// perhaps a msgprint called while
		// trying to do a msgprint because of an
		// error. Ignore.
		return;
	}
	nested++;
	if(e == nil){
		musthavemsgs(nil);
		e = msgedit;
	}
	if(e == nil){
		musthavemsgs(nil);
		e = msgedit;
	}
	if(e != nil){
		va_start(arg, fmt);
		msg = vsmprint(fmt, arg);
		va_end(arg);
		txt = smprint("%s%s", e ? e->text : "", msg);
		free(e->text);
		e->text = txt;
		openpanel(e->gtext, OWRITE|OTRUNC);
		writepanel(e->gtext, txt, strlen(txt));
		closepanel(e->gtext);
		free(msg);
	}
	nested--;
}

char*
cleanpath(char* file, char* dir)
{
	assert(file && file[0]);
	if(file[0] != '/' && dir != nil)
		file = smprint("%s/%s", dir, file);
	else
		file = estrdup(file);
	return cleanname(file);
}

char*
filedir(char* file)
{
	Dir*	d;
	char*	s;

	file = estrdup(file);
	d = dirstat(file);
	if(d && !(d->qid.type&QTDIR)){
		s = utfrrune(file, '/');
		if(s)
			*s = 0;
	}
	free(d);
	return file;
}

void
cleanedit(Edit* e, Dir* d)
{

	assert(e->sts != Stemp);
	e->sts = Sclean;
	if(d)
		e->qid = d->qid;
	else {
		d = dirstat(e->name);
		if(d){
			e->qid = d->qid;
			free(d);
		}
	}
}

static char*
gettagops(char* tag)
{
	char*	s;

	if (tag == nil || *tag == 0)
		return "";
	if (s = utfrune(tag, ']'))
		return s+1;
	for(s = tag; *s == ' ' || *s == '\t'; s++)
		;
	if (*s == 0)
		return "";
	while( *s && *s != ' ' && *s != '\t')
		s++;
	return s;
}

void
updatetag(Edit* e, int keep)
{
	static char* home = nil;
	char*	ops;
	char*	utag;
	char*	nutag;
	char*	s;
	long	l;

	assert(e);
	if (home == nil)
		home = getenv("home");
	if(e->gtag == nil)	// No tag panel!
		return;
	nutag = emalloc(Tagmax);
	s = strecpy(nutag, nutag+Tagmax, e->name);
	if (keep){
		utag = readallpanel(e->gtag, &l);
		ops = gettagops(utag);
		strecpy(s, nutag+Tagmax, ops);
		free(utag);
	} else {
		if(e->qid.type&QTDIR){
			if (!strcmp(e->name, home))
				ops = " Exit Done Get ";
			else
				ops = " Done Get ";
		}else
			ops = " Done Put Get ";
		s = strecpy(s, nutag+Tagmax, ops);
		tagcmds(e->name, s, Tagmax - (s - nutag));
	}
	l = strlen(nutag);
	openpanel(e->gtag, OWRITE|OTRUNC);
	if(writepanel(e->gtag, nutag, l) != l)
		fprint(2, "%s: updatetag: %r\n", argv0);
	closepanel(e->gtag);
	free(nutag);
}

char*
gettagpath(Edit* e)
{
	long	l;
	char*	t;
	char*	s;

	if(e->gtag == nil || openpanel(e->gtag, OREAD) < 0)
		return nil;
	t = emalloc(Tagmax);
	l = readpanel(e->gtag, t, Tagmax-1);
	if(l >= 0){
		t[l] = 0;
		s = utfrune(t, ' ');
		if (s)
			*s = 0;
		s = utfrune(t, '\t');
		if (s)
			*s = 0;
	} else {
		free(t);
		t = nil;
	}
	closepanel(e->gtag);
	return t;
}

int
getsts(Panel* gtext, Sts* e)
{
	char	ctls[512];
	char*	s;
	char*	q;
	int	rc;

	openpanelctl(gtext);
	rc = readpanelctl(gtext, ctls, sizeof(ctls)-1);
	closepanelctl(gtext);
	if(rc <= 0){
		fprint(2, "%s: getsts: %r\n", argv0);
		return e->sts;
	}
	ctls[rc] = 0;
	if(e->sts != Stemp)
		if(utfutf(ctls, "dirty"))
			e->sts = Sdirty;
		else
			e->sts = Sclean;
	s = utfutf(ctls, "mark ");
	if(s != nil){
		s += 5;
		e->mark = strtod(s, &q);
	}
	s = utfutf(ctls, "sel ");
	if(s != nil){
		s += 4;
		e->ss = strtod(s, &q);
		e->se = strtod(q, &s);
	}
	return e->sts;
}

static int
isfixedfont(char* path)
{
	char*	s;

	s = utfrrune(path, '/');
	if(s == nil)
		s = path;
	if(!strcmp(s, "mkfile"))
		return 1;
	s = utfrrune(path, '.');
	if(s)
		path = s;
	return !strcmp(path, ".c") || !strcmp(path, ".h") || !strcmp(path, ".y");
}

void
updatetext(Edit* e)
{
	long	l;
	char*	ctl;

	if(openpanel(e->gtext, OWRITE|OTRUNC) < 0){
		msgprint(nil, "openpanel: %s: %r\n", e->name);
		return;
	}
	if(e->text != nil && (l=strlen(e->text)) != 0){
		if(writepanel(e->gtext, e->text, l) != l){
			dprint("%s: %r\n", e->name);
			msgprint(nil, "writepanel: %s: %r\n", e->name);
		} else
			dprint("ox: writepanel %s: %ld bytes\n", e->name, l);
	}
	if(isfixedfont(e->name) || (e->qid.type&QTDIR) || (e->text != nil && !strncmp(e->text, "#!/", 3)))
		ctl = "font T\nsel 0 0\n";
	else
		ctl = "sel 0 0\n";
	openpanelctl(e->gtext);
	panelctl(e->gtext, ctl);
	closepanelctl(e->gtext);
	closepanel(e->gtext);
}

static int
cmpent(void* a, void* a2)
{
	Dir*	s1 = a;
	Dir*	s2 = a2;

	return strcmp(s1->name, s2->name);
}

static void
fmtdir(Edit* e, Dir* dents, int ndents)
{
	int	i, col;
	char*	s;
	char*	se;
	int	l;
	int	maxw, wl;

	free(e->text);
	e->text = nil;
	if(ndents == 0){
		e->text = estrdup("no files\n");
		return;
	}
	maxw = 0;
	for(i = 0; i < ndents; i++)
		if (maxw < strlen(dents[i].name) + 1)
			maxw = strlen(dents[i].name) + 1;
	l = (maxw+1) * ndents + 2;
	s = e->text = emalloc(l);
	se = e->text + l;
	col = 0;
	for(i = 0; i < ndents; i++){
		s = strecpy(s, se, dents[i].name);
		wl = strlen(dents[i].name);
		if(dents[i].qid.type&QTDIR){
			*s++ = '/';
			wl++;
		}
		col++;
		if(col == 5 || i == ndents - 1){
			*s++ = '\n';
			col = 0;
		} else {
			while(wl < maxw){
				*s++ = ' ';
				wl++;
			}
			*s++ = '\t';
		}
		*s = 0;
	}
}

static void
readdir(Edit* e)
{
	Dir*	dents;
	int	ndents;
	int	fd;

	fd = open(e->name, OREAD);
	if(fd < 0){
		msgprint(nil, "reading %s: %r\n", e->name);
		e->text = estrdup("can't read");
		return;
	}
	ndents = dirreadall(fd, &dents);
	close(fd);
	if (ndents > 0)
		qsort(dents, ndents, sizeof(Dir), cmpent);
	if (ndents >= 0)
		fmtdir(e, dents, ndents);
	free(dents);
}

int
loadfile(Edit* e, char* file)
{
	Dir*	d;

	assert(e->sts != Stemp);
	d = dirstat(file);
	if(d == nil)
	free(e->name);
	e->name = estrdup(file);
	free(e->dir);
	e->dir = filedir(e->name);
	free(e->text);
	e->text = nil;
	if(d != nil)
		if(d->qid.type&QTDIR)
			readdir(e);
		else
			e->text = readfstr(e->name);
	else
		msgprint(nil, "%s: new file\n", file);
	if(e->text == nil)
		e->qid.path = 0;
	dprint("ox: loaded %ld bytes into %p\n",
		e->text ? strlen(e->text) : 0, e->text);
	cleanedit(e, d);
	free(d);
	return 1;
}

static void
addedit(Edit* e)
{
	int	i;

	for(i = 0; i < nedits; i++)
		if(edits[i] == nil){
			edits[i] = e;
			return;
		}

	if((nedits%16) == 0)
		edits = erealloc(edits, (nedits+16)*sizeof(Edit*));
	edits[nedits++] = e;
}

static Edit*
newedit(char* file, int temp)
{
	Edit*	e;
	char*	s;

	assert(!temp || file[0] == '[');
	e = emalloc(sizeof(Edit));
	memset(e, 0, sizeof(Edit));
	e->atime = time(nil);
	if(temp){
		e->name = estrdup(file);
		e->text = estrdup("");
		e->sts = Stemp;
		if(!strcmp(file, "[Cmds]"))
			e->dir = estrdup(homedir);
		else {
			file = estrdup(file);
			if(s = utfrrune(file, ']'))
				*s = 0;
			if(s = utfrune(file, ' '))
				*s = 0;
			e->dir = filedir(file+1);
			free(file);
		}
		addedit(e);
	} else
		if(loadfile(e, file))
			addedit(e);
		else {
			freeedit(e);
			e = nil;
		}
	return e;
}

void
deledit(Edit* e)
{
	int	i;

	dprint("deledit %p: ", e);
	for(i = 0; i < nedits; i++)
		if(edits[i] == e){
			edits[i] = nil;
			dprint("%s\n", e->name);
			freeedit(e);
			return;
		}
	abort();
}

static int
lrueditcmp(void* a1, void* a2)
{
	Edit**	e1 = a1;
	Edit**	e2 = a2;

	if(*e1 == *e2)
		return 0;
	/* place nil entries at the start.
	 */
	if(*e1 == nil)
		return -1;
	if(*e2 == nil)
		return 1;
	return (*e1)->atime - (*e2)->atime;
}

static void
cleanpolicy(void)
{
	int	i;
	long	now;
	Edit*	e;
	int	nused;

	qsort(edits, nedits, sizeof(edits[0]), lrueditcmp);
	if(debug)
		dumpedits();
	nused = 0;
	for(i = 0; i < nedits; i++)
		if(edits[i])
			nused++;
	if(nused <= Dnrun)
		return;
	now = time(nil);
	for(i = 0; i < nedits; i++)
		if(e = edits[i])
			if(e->sts == Stemp && cdone(e, 0, nil, 0)){
				if(--nused <= Dnwins)
					break;
			} else
				if(e->sts != Sdirty && now - e->atime > Dtime)
				if(cdone(e, 0, nil, 0) && --nused <= Dnwins)
					break;
}


static void
updatewhere(char* path)
{
	char*	p;

	free(where);
	where = estrdup(path);
	p = utfutf(where, "/col:ox.");
	assert(p);
	*p = 0;
}

static void
closefile(Edit* e, int exiting)
{
	dprint("ox: closefile for %s\n", e->name);
	if(!exiting)
	if(e->sts != Stemp && !(e->qid.type&QTDIR))
	if(getsts(e->gtext, e) == Sdirty){
		// BUG: should locate at /tmp
		// the backup file for the text graph.
		// it's named BCK.<graph name> 
		// Then should move it to <e->name>.unsaved
		return;
	}
	if(e->gtext)
		removepanel(e->gtext);
	e->gtext = nil;
	if(e->gtag)
		removepanel(e->gtag);
	e->gtag = nil;
	if(e->gcol)
		removepanel(e->gcol);
	e->gcol = nil;
	if (e->pid == 0){
		deledit(e);
		musthaveedits();
		if (debug)dumpedits();
	}
	/* Otherwise, there is an xcmdoutthread
	 *  using the edit. That thread will close the edit
	 * when safe to do so.
	 */
}

Edit*
editfile(char* path, int temp)
{
	Edit*	e;
	Panel*	w;
	static	int nb;

	if(e = findedit(path))
		return e;
	cleanpolicy();
	e = newedit(path, temp);
	if (e == nil)
		return nil;

	w = createpanel("ox", "col", where);
	if(w == nil){
		if(where != nil){
			/* Maybe where was wrong for now */
			free(where);
			where = nil;
			w = createpanel("ox", "col", where);
		}
		if(w == nil)
			goto fail;
	}
	if(where == nil && w->repl != nil)
		updatewhere(w->repl->path);
	if(debug)
		dumpedit(e);
	nb++;
	e->gcol = w;
	e->gcolname = estrdup(strrchr(w->repl->path, '/')+1);
	e->gtag = createsubpanel(e->gcol, "tag:oxedit");
	if(e->gtag == nil)
		goto fail;
	updatetag(e, 0);
	e->gtext= createsubpanel(e->gcol, "text:oxedit");
	if(e->gtext == nil)
		goto fail;
	updatetext(e);
	if(debug)
		dumpedits();
	if (space)
		panelctl(w, "space %d", space);
	closepanelctl(w);
	return e;
fail:
	if(w != nil)
		closepanelctl(w);
	closefile(e, 0);
	return nil;
}

void
look(Edit* e, char* what, char* path)
{
	char*	p;
	char*	addr;

	if (!what)
		return;

	dprint("%s: look %s\n", e->name, what);
	p = cleanpath(what, e->dir);
	addr = nil;

	/* 1. Try to open (and set address)
	 */
	if(access(p, AEXIST) < 0){
		addr = utfrune(p, ':');
		if (addr != nil){
			if (what[0] == ':')
				goto Search;
			*addr++ = 0;
			if (access(p, AEXIST) < 0){
				free(p);
				p = addr = nil;
			}
		} else {
			free(p);
			p = nil;
		}
	}
	if (p != nil){
		evhistory("ox", "look", p);
		if (mustplumb(p))
			goto Plumb;
		e = editfile(p, 0);
		if (addr){
			*--addr = ':';
			openpanelctl(e->gtext);
			panelctl(e->gtext, "search %s\n", addr);
			closepanelctl(e->gtext);
		}
		openpanelctl(e->gcol);
		panelctl(e->gcol, "show\nnomin\n");
		closepanelctl(e->gcol);
		free(p);
		return;
	}

	/* 2. Try to plumb
	 */
Plumb:
	if (plumblook(e->dir, what)){
		free(p);
		return;
	}
Search:
	/* 3. Try searching for the string
	 * BUG: should search in path, not in e->gtext.
	 */
	USED(path);
	if (what != nil){
		openpanelctl(e->gtext);
		panelctl(e->gtext, "search %s\n", what);
		closepanelctl(e->gtext);
	}
	free(p);
}

static void
interrupt(int pid)
{
	char	fn[40];

	seprint(fn, fn+sizeof(fn), "/proc/%d/notepg", pid);
	writefstr(fn, "interrupt");
}

static void
wreplctl(char* path, void* buf, long l)
{
	char* f;

	f = smprint("%s/ctl", path);
	writef(f, buf, l);
	free(f);
}

static void
textevent(Edit* e, char* path, char* ev, char* arg, int istag)
{
	char*	narg;
	char*	s;

	if(!istag && !strcmp(ev, "interrupt")){
		e->atime = time(nil);
		if(e->pid != 0)
			interrupt(e->pid);
	} else if(!istag && !strcmp(ev, "dirty")){
		e->atime = time(nil);
		if(e->sts != Stemp)
			e->sts = Sdirty;
	} else if(!strcmp(ev, "look") && arg != nil){
		e->atime = time(nil);
		look(e, arg+12, path);
	} else if(!strcmp(ev, "args") && arg != nil){
		e->atime = time(nil);
		wreplctl(path, "cut\npaste\n", 3+1+5+1);
		s = readfstr("/dev/snarf");
		narg = smprint("%s %s", arg + 12, s ? s : "");
		free(s);
		run(e, narg, istag, path);
		free(narg);
	} else if(!strcmp(ev, "exec") && arg != nil){
		e->atime = time(nil);
		s = smprint("cd %s ; %s", e->dir, arg+12);
		evhistory("ox", "exec", s);
		free(s);
		run(e, arg + 12, istag, path);
	} else if (!istag && !strcmp(ev, "exit")){
		closefile(e, 1);
	}
}

static Panel*
openxpanel(char* dir)
{
	Panel*	g;

	g = emalloc(sizeof(Panel));
	memset(g, 0, sizeof(Panel));
	g->nrepl = 1;
	g->repl = emalloc(sizeof(Repl));
	memset(g->repl, 0, sizeof(Repl));
	g->repl->path = estrdup(dir);
	return g;
}

static void
closexpanel(Panel* g)
{
	free(g->repl->path);
	free(g->repl);
	free(g);
}

void
externrunevent(char* path, char* ev, char* arg)
{
	char*	narg;
	Panel*	gtext;
	char*	s;

	narg = nil;
	if(!strcmp(ev, "args") && arg){
		gtext = openxpanel(path);
		wreplctl(path, "cut\npaste\n", 3+1+5+1);
		s = readfstr("/dev/snarf");
		narg = smprint("%s %s", arg, s ? s : "");
		arg = narg;
		free(s);
	} else if(!strcmp(ev, "exec"))
		gtext = openxpanel(path);
	else
		return;
	dprint("editrun %s\n", arg);
	if(!editrun(gtext, "/tmp", arg, path))
		xcmd(nil, "/tmp", arg, nil, nil, nil);
	free(narg);
	closexpanel(gtext);
}

static int
cmdonsel(Oev* ev)
{
	if(!strcmp(ev->ev, "args"))
		return 1;
	if(!strcmp(ev->ev, "exec") && ev->arg)
	if(strchr(":|<>", ev->arg[12]) || !strncmp(ev->arg+12, "E ", 2))
		return 1;
	return 0;
}

static void
event(Oev* ev)
{
	int	i;
	Edit*	e;
	char*	p;
	char*	s;

	if(cmdonsel(ev)){
		free(ev->path);
		ev->path = nil;
		ev->path = readfstr("/dev/sel");
		dprint("edit event for %s\n", ev->path);
		if(ev->path == nil){
			msgprint(nil, "No selection. /dev/sel: %r\n");
			return;
		}
	}
	if(!strcmp(ev->ev, "path")){
		s = smprint("/devs%s", ev->arg);
		updatewhere(s);
		free(s);
		return;
	}
	e = nil;
	p = nil;
	for(i = 0; i < nedits; i++)
		if(edits[i] && edits[i]->gcolname)
		if(p = strstr(ev->path, edits[i]->gcolname)){
			e = edits[i];
			break;
		}
	if(e != nil)
		textevent(e, ev->path, ev->ev, ev->arg, strstr(p, "/tag:") != nil);
	else {
		dumpedits();
		dprint("event w/o edit %s %s (path %s)\n", ev->ev, ev->arg, ev->path);
		externrunevent(ev->path, ev->ev, ev->arg ? ev->arg + 12 : nil);
	}
}

static void
usage(void)
{
	fprint(2, "usage: %s [-d] [-s spacenb] [-r machine] [file]\n", argv0);
	sysfatal("usage");
}

void
threadmain(int argc, char* argv[])
{
	Channel* ec;
	Oev	e;
	extern int omerodebug;
	char*	remote;

	remote = nil;
	ARGBEGIN{
	case 's':
		space = atoi(EARGF(usage()));
		break;
	case 'd':
		debug++;
		break;
	case 'r':
		remote = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;
	if (argc > 1)
		usage();
	if (debug> 1)
		omerodebug++;
	if (remote != nil){
		fprint(2, "remote ox: space %d machine %s omero %s\n", space, remote, getenv("omero"));
		threadexits(nil);
	}
	inittagcmds();
	homedir = getenv("home");
	cleanname(homedir);
	plumbinit();
	cmdinit();
	ec = omeroeventchan(nil);
	editfile(homedir, 0);
	while(recv(ec, &e) != -1){
		dprint("user: %s %s %s\n", e.path, e.ev, e.arg);
		event(&e);
		clearoev(&e);
	}
}
