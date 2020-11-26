#include <u.h>
#include <libc.h>
#include <thread.h>
#include <error.h>
#include <b.h>
#include <omero.h>

/*
 * Error semantics:
 * 1. Replicas are removed on I/O errors.
 * 2. I/O fails when all the replicas failed.
 * 3. Errors are ignored on closes and removes
 *
 * Locking:
 * - glock held while changing list of panels
 * - g->Lock held while changing g->repl
 * - Events keep a ref to a panel. A panel is
 *   removed when the user asks for it. But the data
 *   structure is released only when all pending
 *   events pointing to the panel have been processed.
 */

static Panel*	panels;
static QLock	glock;	// for all panels.

#define ddump	if(omerodebug>1)paneldump
#define dprint	if(omerodebug)fprint

void
paneldump(int fd)
{
	Panel* g;
	Repl* r;

	qlock(&glock);
	for(g = panels; g; g = g->next){
		qlock(g);
		fprint(fd, "%s %ld ref:\n", g->name, g->ref);
		for (r = g->repl; r; r = r->next)
			fprint(fd, "\t%s dfd=%d cfd=%d\n",
				r->path, r->fd[Fdata], r->fd[Fctl]);
		qunlock(g);
	}
	qunlock(&glock);
}

static int
prefix(char* pref, int prefl, char* path)
{
	if (!strncmp(pref, path, prefl))
	if (path[prefl] == '/' || path[prefl] == 0)
		return 1;
	return 0;
}

static Panel*
panelalloc(char* name)
{
	Panel*	g;

	g = emalloc(sizeof(Panel));
	memset(g, 0, sizeof(*g));
	g->next = panels;
	g->name = estrdup(name);
	assert(g->name);
	panels = g;
	return g;
}

/* Used by newpanel and by unpackevent.
 * This adds a new Ref for Panel.
 */
Panel*
findpanel(char* n, int mkit)
{
	Panel* g;

	qlock(&glock);
	for(g = panels; g; g = g->next)
		if (!strcmp(g->name, n))
			break;
	if (g && g->gone){
		qunlock(&glock);
		return nil;
	}
	if (mkit && !g)
		g = panelalloc(n);
	if (g != nil)	
		incref(g);
	qunlock(&glock);
	return g;
}

Repl*
findrepl(Panel* g, char* path, int mkit)
{
	Repl* r;

	qlock(g);
	for (r = g->repl; r; r = r->next)
		if (!strcmp(r->path, path))
			break;
	if (!r && mkit){
		r = emalloc(sizeof(Repl));
		r->path= strdup(path);
		r->fd[0] = r->fd[1] =-1;
		r->next = g->repl;
		g->repl = r;
		g->nrepl++;
	}
	qunlock(g);
	return r;
}

/* Creates a new panel or returns a
 * ref to one created by an event.
 * This means one Ref into Panel.
 */
Panel*
newpanel(char* path, int mkit)
{
	Panel*	g;
	char*	name;

	name = strrchr(path, '/');
	if (!name){
		werrstr("newpanel: path must be absolute");
		return nil;
	}
	name++;
	if (g = findpanel(name, mkit))
		findrepl(g, path, 1);
	return g;
}


Panel*
mkpanel(char* path)
{
	int	fd;
	Panel*	g;

	fd = create(path, OREAD, 0775|DMDIR);
	if (fd < 0)
		return nil;
	close(fd);
	g =  newpanel(path, 1);
	dprint(2, "mkpanel: %s\n", path);
	return g;
}

static void
rmrepl(Repl* r)
{
	char*	path;

	if (r->fd[0] >= 0)
		close(r->fd[0]);
	if (r->fd[1] >= 0)
		close(r->fd[1]);
	r->fd[0] = r->fd[1] = -1;
	path = smprint("%s/ctl", r->path);
	assert(path);
	remove(path);
	free(path);
	path = smprint("%s/data", r->path);
	assert(path);
	remove(path);
	free(path);
	remove(r->path);
}

Panel*
createsubpanel(Panel* g, char* name)
{
	Repl*	r;
	char*	path;
	Panel*	ng;
	ulong	rnd;
	Panel*	rg;
	Repl**	rl;
	char	e[100];

	rnd = truerand()%10000;
	rg = nil;
	qlock(g);
	e[0] = 0;
	for (rl = &g->repl; r = *rl; ){
		path = smprint("%s/%s.%uld", r->path, name, rnd);
		assert(path);
		ng = mkpanel(path);
		dprint(2, "createsubpanel %s %p\n", path, ng);
		free(path);
		if (ng == nil){
			dprint(2, "createsubpanel: rmrepl: %s\n", r->path);
			rerrstr(e, sizeof(e));
			rmrepl(r);
			*rl = r->next;
			free(r->path);
			free(r);
			g->nrepl--;
		} else {
			ng->evc = g->evc;
			if (rg == nil)
				rg = ng;
			rl = &r->next;
		}
	}
	qunlock(g);
	if (rg == nil)
		werrstr("createsubpanel: %s", e);
	return rg;
}

char*
panelpath(Panel* g)
{
	char*	p;

	qlock(g);
	if (g->repl != nil && g->repl->path != nil)
		p = strdup(g->repl->path);
	else
		p = nil;
	qunlock(g);
	return p;
}

static void
panelfree(Panel* g)
{
	Repl*	gr;
	Repl*	nr;

	if (g == nil)
		return;
	free(g->name);
	for(gr = g->repl; gr; gr = nr){
		nr = gr->next;
		assert(gr->fd[Fctl] < 0);
		assert(gr->fd[Fdata] < 0);
		free(gr->path);
		free(gr);
	}
	free(g);
}

static void
unlinkpanel(Panel* g)
{
	Panel**	gl;

	for(gl = &panels; *gl; gl = &(*gl)->next){
		if (*gl == g){
			*gl = g->next;
			break;
		}
	}
}

void
removepanel(Panel* g)
{
	Repl*	r;
	int	wasgone;

	qlock(g);
	wasgone = g->gone;
	g->gone = 1;
	for (r = g->repl; r; r = r->next){
		dprint(2, "removepanel: rmrepl: %s\n", r->path);
		rmrepl(r);
	}
	qunlock(g);

	if (!wasgone && decref(g) > 0)	// must have events pending.
		return;
	qlock(&glock);
	unlinkpanel(g);
	panelfree(g);
	qunlock(&glock);
}

static int
_openpanel(Panel* g, int omode, int what)
{
	Repl*	r;
	Repl**	rl;
	char*	path;
	char	e[100];

	qlock(g);
	seprint(e, e+sizeof(e), "%s: no replicas", g->name);
	if (g->repl == nil)
		dprint(2, "openpanel: %s: no replicas\n", g->name);
	for (rl = &g->repl; r = *rl; ){
		if (r->fd[what] < 0){
			if (what == Fctl)
				path = smprint("%s/ctl", r->path);
			else
				path = smprint("%s/data", r->path);
			r->fd[what] = open(path, omode);
			free(path);
			if (r->fd[what] < 0){
				rerrstr(e, sizeof(e));
				dprint(2, "openpanel: rmrepl: %s: %r\n", r->path);
				rmrepl(r);
				*rl = r->next;
				free(r->path);
				free(r);
				g->nrepl--;
				continue;
			}
		}
		rl = &r->next;
	}
	qunlock(g);
	if (g->repl == nil){
		werrstr("openpanel: %s", e);
		return -1;
	}
	return 1;
}

static void
_closepanel(Panel* g, int what)
{
	Repl*	r;

	qlock(g);
	for (r = g->repl; r; r = r->next){
		if (r->fd[what] >= 0){
			close(r->fd[what]);
			r->fd[what] = -1;
		}
	}
	qunlock(g);
}

static long
_readpanel(Panel* g, int what, void* buf, long len)
{
	Repl*	r;
	Repl**	rl;
	long	rc, nr;
	char	e[100];

	qlock(g);
	rc = -1;
	seprint(e, e+sizeof(e), "%s: not open", g->name);
	for (rl = &g->repl; r = *rl; )
		if (r->fd[what] >= 0){
			nr = read(r->fd[what], buf, len);
			if (nr >= 0){
				rc = nr;
				break;
			}
			rerrstr(e, sizeof(e));
			dprint(2, "_readpanel: rmrepl: %s: %r\n", r->path);
			rmrepl(r);
			*rl = r->next;
			free(r->path);
			free(r);
			g->nrepl--;
		} else
			rl = &r->next;
	qunlock(g);
	if (rc < 0)
		werrstr("_readpanel: %s", e);
	return rc;
}

Dir*
dirstatpanel(Panel* g)
{
	Repl*	r;
	Repl**	rl;
	Dir*	d;
	char	e[100];
	char*	path;

	qlock(g);
	seprint(e, e+sizeof(e), "%s: no replicas", g->name);
	d = nil;
	for (rl = &g->repl; r = *rl; ){
		if (r->fd[Fdata] >= 0)
			d = dirfstat(r->fd[Fdata]);
		else {
			path = smprint("%s/data", r->path);
			d = dirstat(path);
			free(path);
		}
		if (d != nil)
			break;
		rerrstr(e, sizeof(e));
		dprint(2, "dirstatpanel: rmrepl: %s: %r\n", r->path);
		rmrepl(r);
		*rl = r->next;
		free(r->path);
		free(r);
		g->nrepl--;
	}
	qunlock(g);
	if (d == nil)
		werrstr("_readpanel: %s", e);
	return d;
}

void*
readallpanel(Panel* g, long* l)
{
	Dir*	d;
	char*	buf;

	d = dirstatpanel(g);
	if (d == nil){
		*l = -1;
		return nil;
	}
	buf = malloc(d->length+1);
	*l = d->length;
	free(d);
	if (buf == nil){
		*l = -1;
		return nil;
	}
	if (openpanel(g, OREAD) < 0){
		free(buf);
		return nil;
	}
	*l = readpanel(g, buf, *l);
	closepanel(g);
	if (*l < 0){
		free(buf);
		buf = nil;
	}else
		buf[*l] = 0;
	return buf;
}

/* A ctl write error is ignored. Otherwise, a
 * bad ctl would remove the replica thinking that
 * it has been an error accesing the replica files.
 */
static long
_writepanel(Panel* g, int what, void* buf, long len)
{
	Repl*	r;
	Repl**	rl;
	long	wc;
	long	ec;
	char	e[100];

	if (len == 0)
		return 0;
	qlock(g);
	seprint(e, e+sizeof(e), "%s: no replicas", g->name);
	ec = -1;
	for (rl = &g->repl; r = *rl; ){
		if (r->fd[what] >= 0){
			wc = write(r->fd[what], buf, len);
			if (wc != len){
				if (wc < 0)
					rerrstr(e, sizeof(e));
				else
					strcpy(e, "couldn't write all");
				if (what == Fdata){
					rmrepl(r);
					*rl = r->next;
					free(r->path);
					free(r);
					g->nrepl--;
					werrstr("writepanel: %s", e);
					continue;
				}
			} else
				ec = wc;
		}
		rl = &r->next;
	}
	qunlock(g);
	return ec;
}

int
openpanel(Panel* g, int omode)
{
	return _openpanel(g, omode, Fdata);
}

int
openpanelctl(Panel* g)
{
	return _openpanel(g, ORDWR, Fctl);
}

void
closepanel(Panel* g)
{
	_closepanel(g, Fdata);
}

void
closepanelctl(Panel* g)
{
	_closepanel(g, Fctl);
}

long
writepanel(Panel* g, void* buf, long len)
{
	return _writepanel(g, Fdata, buf, len);
}

long
writepanelctl(Panel* g, void* buf, long len)
{
	return _writepanel(g, Fctl, buf, len);
}

long
readpanel(Panel* g, void* buf, long len)
{
	return _readpanel(g, Fdata, buf, len);
}

long
readpanelctl(Panel* g, void* buf, long len)
{
	return _readpanel(g, Fctl, buf, len);
}

vlong
seekpanel(Panel*g, vlong pos, int type)
{
	Repl*	r;
	vlong	rc;
	int	some;

	rc = -1;
	some = 0;
	qlock(g);
	for (r = g->repl; r; r = r->next)
		if (r->fd[Fdata] >= 0){
			some++;
			rc=seek(r->fd[Fdata], pos, type);
		}
	if (!some)
		werrstr("%s: no replicas", g->name);
	qunlock(g);
	return rc;
}

int
panelctl(Panel* g, char* fmt, ...)
{
	char*	ctl;
	va_list	arg;
	long	rc;

	va_start(arg, fmt);
	ctl = vsmprint(fmt, arg);
	va_end(arg);
	rc = writepanelctl(g, ctl, strlen(ctl));
	free(ctl);
	return rc;
}

void
wpanelexcl(Panel* g, char* what, void* buf, long len, Repl* excl)
{
	Repl*	r;
	char*	path;

	qlock(g);
	for(r = g->repl; r; r = r->next){
		if (r != excl){
			path = smprint("%s/%s", r->path, what);
			if (path)
				writef(path, buf, len);
			free(path);
		}
	}
	qunlock(g);
}

void
rpaneldata(Panel* g, Repl* r)
{
	char*	f;
	void*	data;
	long	len;

	if (g == nil || r == nil)
		return;
	qlock(g);
	if (g->repl && g->nrepl >= 2){
		f = smprint("%s/data", r->path);
		data = readf(f, nil, 0, &len);
		free(f);
		if (data != nil){
			qunlock(g);
			wpanelexcl(g, "data", data, len, r);
			free(data);
			return;
		}
	}
	qunlock(g);
}

void
movepanel(char* from, char* to)
{
	Panel*	g;
	Repl*	r;

	qlock(&glock);
	for(g = panels; g; g = g->next){
		qlock(g);
		for (r = g->repl; r; r = r->next)
			if (!strcmp(from, r->path)){
				free(r->path);
				r->path = strdup(to);
				qunlock(g);
				qunlock(&glock);
				return;
			}
		qunlock(g);
	}
	qunlock(&glock);

	/* To debug problems related to races on panel moves
	 */
	fprint(2, "movepanel: no panel: move from %s to %s\nPanels:", from, to);
	qlock(&glock);
	for(g = panels; g; g = g->next){
		fprint(2, "%s:\n", g->name);
		qlock(g);
		for (r = g->repl; r; r = r->next)
			fprint(2, "\t%s\n", r->path);
		qunlock(g);
	}
	qunlock(&glock);

	/* No source replica found. Create the target anyway.
	 */
	if (g = newpanel(from, 0))
		decref(g);	// we don't keep the ref. It's > 1 also.
}
