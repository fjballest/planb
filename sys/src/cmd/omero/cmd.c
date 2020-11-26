#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <draw.h>
#include <mouse.h>
#include <cursor.h>
#include <keyboard.h>
#include <frame.h>
#include <9p.h>
#include <plumb.h>
#include <b.h>
#include "gui.h"
#include "cook.h"
#include <pool.h>

static int	creatingcol;
static int	creatingrow;
static int	deleting;
static int	identifying;
static int	copying;
static File*	movingf;

static void
resetcmd(void)
{
	creatingcol = creatingrow = deleting = identifying = copying = 0;
	movingf = nil;
	argcursor(0);
}

int
within(File* d, File* f)
{
	int	found;
	File**	w;
	File**	l;
	File*	fp;
	Panel*	np;

	found = 0;
	assert(f);
	w = newfilewalk(d);
	for (l = w; fp = *l; l++)
		if (ispanel(fp)){
			found |= (fp == f);
			np = fp->aux;
			if (!found && (np->type == Qcol || np->type == Qrow))
				found |= within(fp, f);
		}
	endfilewalk(w);
	return found;
}

static void
notifymove(File* f, char* old, int nlen)
{
	File**	l;
	File**	w;
	File*	fp;
	Panel*	p;
	char*	npath;

	p = f->aux;
	npath = filepath(f);
	free(p->path);
	p->path = estrdup9p(npath);
	conprint(p->con, "%s%s path %s\001", old, npath + nlen, npath);
	free(npath);
	w = newfilewalk(f);
	for (l = w; fp = *l; l++)
		if (ispanel(fp))
			notifymove(fp, old, nlen);
	endfilewalk(w);
}

static File*
insertpoint(File* f, Point xy)
{
	File**	l;
	File**	w;
	File*	fp;
	Panel*	p;
	Panel*	np;
	File*	last;
	int	m;
	int	v;

	p = f->aux;
	last = nil;
	w = newfilewalk(f);
	for (l = w; fp = *l; l++){
		if (!ispanel(fp))
			continue;
		np = fp->aux;
		if (p->type == Qcol){
			m = (np->rect.min.y + np->rect.max.y)/2;
			v = xy.y;
		} else {
			m = (np->rect.min.x + np->rect.max.x)/2;
			v = xy.x;
		}
		if (v < m)
			break;
		last = fp;
	}
	endfilewalk(w);
	return last;
}

static void
movefile(File* from, File* f, Point xy)
{
	Panel* p;
	File*	at;
	char*	opath;
	char*	npath;

	if (within(from, f))
		return;
	opath = filepath(from->parent);
	detachfile(from);
	p = f->aux;
	if (p->type != Qcol && p->type != Qrow)
		f = f->parent;
	at = insertpoint(f, xy);
	insertfile(from, f, at);
	npath = filepath(f);
	notifymove(from, opath, strlen(npath));
	free(opath);
	free(npath);
	resize();
}

static void
mkpanel(int id, File* f)
{
	Panel*	p;
	char*	s;
	File*	nf;

	p = f->aux;
	if (p->type != Qcol && p->type != Qrow){
		f = f->parent;
		p = f->aux;
	}
	assert(p->type == Qcol || p->type == Qrow);
	s = smprint("%s:%d", (id == Qcol ? "col:" : "row:"), rand());
	nf = newfspanel(f, s, f->uid);
	p = nf->aux;
	p->flags |= Ptag|Playout;
	closefile(nf);
	free(s);
}

void
rmpanel(File* f)
{
	File**	l;
	File**	w;
	File*	fp;

	edprint("rmpanel %s\n", f->name);

	/* Deleting is a dangerous operation.
	 * Make sure despite bugs we won't delete on
	 * following clicks. Safety first.
	 */
	deleting = 0;

	incref(f);
	w = newfilewalk(f);
	for (l = w; fp = *l; l++){
		if (fp->qid.type&QTDIR)
			rmpanel(fp);
		else {
			incref(fp);
			if (removefile(fp) < 0)
				fprint(2, "rmpanel: %r\n");
		}
	}
	endfilewalk(w);
	removefile(f);
}

int
intag(Panel* p, Point xy)
{
	int	ht;

	if (p->flags&Pmore)
		ht = 2*Taght;
	else
		ht = Taght;
	return ptinrect(xy, Rect(p->rect.min.x, p->rect.min.y,
				p->rect.min.x+Tagwid,
				p->rect.min.y+ht));
}

int
wcommand(char* c)
{
	char*	s;

	edprint("command: %s\n", c);
	if (!strcmp(c, "Col"))
		creatingcol = 1;
	else if (!strcmp(c, "Row"))
		creatingrow = 1;
	else if (!strcmp(c, "Sel"))
		identifying = 1;
	else if (!strcmp(c, "Del"))
		deleting = 1;
	else if (!strcmp(c, "Arg"))
		copying = 1;
	else if (!strcmp(c, "Ox")){
		sendp(startc, estrdup9p(Ox));
		return 1;
	} else if (!strncmp(c, "Ox ", 3)){
		c += 3;
		while(*c == ' ')
			c++;
		s = smprint("%s %s", Ox, c);
		sendp(startc, s);
		return 1;
	} else
		return 0;
	argcursor(1);
	return 1;
}

static void
snarfpath(File* f, char* pref)
{
	Panel*	p;
	char*	s;

	p = f->aux;
	if (pref)
		s=smprint("%s /devs%s", pref, p->path);
	else
		s=smprint("/devs%s", p->path);
	writefstr("/dev/snarf", s);
	free(s);
}

static int
plumbexec(char* dir, char* arg)
{
	static int plumbsendfd = -1;
	Plumbmsg*m;

	if (plumbsendfd < 0)
		plumbsendfd = plumbopen("send", OWRITE|OCEXEC);
	if (plumbsendfd < 0){
		fprint(2, "plumbopen: send: %r\n");
		return 0;
	}
	m = malloc(sizeof(Plumbmsg));
	if (m == nil)
		return 0;
	m->src = estrdup9p(argv0);
	m->dst = estrdup9p("exec");
	m->wdir= estrdup9p(dir);
	m->type = estrdup9p("text");
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

static void
copypath(File* f)
{
	Panel*	p;
	char*	s;
	char*	cmd;

	p = f->aux;
	s = readfstr("/dev/snarf");
	cmd = smprint("%s /devs%s", s, p->path);
	plumbexec("/", cmd);
	free(cmd);
	free(s);
}

void
tagmousecmd(File *f, Cmouse* m, Channel* mc)
{
	Panel*	p;
	Panel*	pfp;
	File*	nf;
	int	innerok;

	p = f->aux;
	if (!m->buttons)
		return;
	innerok = 0;
	if (m->buttons == 1){
		recv(mc, m);
		if (!m->buttons){
			if (intag(p, m->xy))
			if (p->type == Qcol || p->type == Qrow){
				if (p->type == Qcol)
					p->type = Qrow;
				else
					p->type = Qcol;
				p->flags |= Predraw;
				resize();
			}
		} else {
			if (m->buttons == 3 && intag(p, m->xy)){
				do {
					recv(mc, m);
				} while(m->buttons != 0);
				argcursor(1);
				snarfpath(f, "Ocp" );
				argcursor(0);
				return;
			}
			if (m->buttons == 5){
				do {
					recv(mc, m);
				} while(m->buttons != 0);
				argcursor(1);
				copypath(f);
				argcursor(0);
				return;
			}
		slide:
			if (!intag(p, m->xy))
				return;
			argcursor(1);
			if (cookslide(m, mc)){
				argcursor(0);
				nf = pointinpanel(m->xy, innerok);
				if (!innerok)
					nf = paneltop(nf);
				edprint("move start at %s\n", f->name);
				edprint("move end at %s\n", nf->name);
				if (nf != f){
					pfp = f->parent->aux;
					p = nf->aux;
					p->flags |= Predraw;
					pfp->flags|= Predraw;
					movefile(f, nf, m->xy);
				}
			} else
				argcursor(0);
		}
	} else if (m->buttons == 2 && intag(p, m->xy)){
		recv(mc, m);
		if (!m->buttons){
			maxwin(f);
			resize();
		} else {
			innerok = 1;
			goto slide;
		}
	} else  if (m->buttons == 4 && intag(p, m->xy)){
		if (cookclick(m, mc)){
			if (!(p->flags&Ptop)){
				p->flags |= Phide;
				pfp = f->parent->aux;
				pfp->flags |= (Pmore|Predraw);
				resize();
			}
		}
	}
}

int
mousecmdarg(File* f, Cmouse* m, Channel* mc)
{
	Panel*	p;
	char*	s;
	char*	ns;
	int	istop;

	if (creatingcol || creatingrow || deleting || identifying || copying)
	if (m->buttons == 1)
	if (cookclick(m, mc)){
		f = pointinpanel(m->xy, 1);
		if (creatingcol){
			resetcmd();
			mkpanel(Qcol, f);
			resize();
			return 1;
		}
		if (creatingrow){
			resetcmd();
			mkpanel(Qrow, f);
			resize();
			return 1;
		}
		if (deleting){
			resetcmd();
			p = f->aux;
			istop = (p->flags&Ptop);
			if (istop)
				sysfatal("killed by user");
			if ((p->type != Qrow && p->type != Qcol) || hastag(f)){
				p = f->parent->aux;
				p->flags |= Predraw;
				rmpanel(f);
			}
			resize();
			return 1;
		}
		if (identifying){
			resetcmd();
			snarfpath(f, nil);
			return 1;
		}
		if (copying){
			resetcmd();
			p = f->aux;
			s = readfstr("/dev/snarf");
			ns = smprint("%s /devs%s", s, p->path);
			writefstr("/dev/snarf", ns);
			free(s);
			free(ns);
			return 1;
		}
	}
	return 0;
}




