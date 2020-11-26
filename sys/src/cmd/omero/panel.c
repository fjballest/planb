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
#include "cook.h"

typedef struct Fent  Fent;

struct Fent{
	Font*	font;
	char*	name;
};

static char* myaddr;

static void xinit(Panel*);
static void xterm(Panel*);
static long xread(Panel* p, void* buf, long cnt, vlong off);
static long xwrite(Panel* p, void* buf, long cnt, vlong off);
static void xdraw(Panel* p, int resize);
static void xmouse(Panel* p, Cmouse* m, Channel* mc);
static void xkeyboard(Panel*, Rune);

static void 
binit(Panel*)
{
	abort();
}

static void 
bterm(Panel*)
{
	abort();
}

static int  
bctl(Panel*, char*)
{
	abort();
	return -1;
}

static long  
battrs(Panel*, char*, long)
{
	abort();
	return -1;
}

static long 
bread(Panel*, void*, long , vlong)
{
	abort();
	return -1;
}

static long 
bwrite(Panel*, void*, long, vlong)
{
	abort();
	return -1;
}

static void 
bdraw(Panel*, int)
{
	abort();
}

static void 
bmouse(Panel*, Cmouse*, Channel*)
{
	abort();
}

static void 
bkeyboard(Panel*, Rune)
{
	abort();
}


Pops nilops = {
	.pref = "/BUG/",
	.init = binit,
	.term = bterm,
	.ctl = bctl,
	.attrs= battrs,
	.read = bread,
	.write= bwrite,
	.draw = bdraw,
	.mouse = bmouse,
	.keyboard = bkeyboard,
};

Pops rowops = {
	.pref = "row:",
	.init = xinit,
	.term = xterm,
	.ctl = genctl,
	.attrs= genattrs,
	.read = xread,
	.write= xwrite,
	.draw = xdraw,
	.mouse = xmouse,
	.keyboard = xkeyboard,
};

Pops colops = {
	.pref = "col:",
	.init = xinit,
	.term = xterm,
	.ctl = genctl,
	.attrs= genattrs,
	.read = xread,
	.write= xwrite,
	.draw = xdraw,
	.mouse = xmouse,
	.keyboard = xkeyboard,
};

static void
xdraw(Panel*, int)
{
}

static Font*
getfont(char* name)
{

	switch(name[0]){
	case 'R':
	case 'r':
		return fonts[FR];
	case 'B':
	case 'b':
		return fonts[FB];
	case 'L':
	case 'l':
		return fonts[FL];
	case 'T':
	case 't':
		return fonts[FT];
	case 'S':
	case 's':
		return fonts[FS];
	default:
		return nil;
	}
}

static int
paneltype(char* name)
{
	int	i;

	for (i = 0; panels[i] != nil; i++)
		if (strstr(name, panels[i]->pref) == name)
			return i;
	return -1;
}

long
genreadbuf(void* data, long count, vlong off, void *s, long n)
{
	long	r;

	r = count;
	if(off >= n)
		return 0;
	if(off+r > n)
		r = n - off;
	memmove(data, (char*)s+off, r);
	return r;
}


static long
xread(Panel*, void* , long , vlong )
{
	return 0;
}

static long
xwrite(Panel*, void* , long , vlong )
{
	return 0;
}

static void
xmouse(Panel* p, Cmouse* m, Channel* mc)
{
	tagmousecmd(p->file, m, mc);
}

static void
xinit(Panel* p)
{
	p->flags |= Ptag;
}

static void
xterm(Panel*)
{
}

static int
intagy(Panel* p, Point xy)
{
	return xy.y >= p->rect.min.y &&
		xy.y <= p->rect.min.y + 2 * Taght;
}

static void
xkeyboard(Panel* p, Rune r)
{
	File*	f;

	f = p->file;
	if (r == Kup || r == Kleft){
		if (intagy(p, lastxy))
			fullwin(f);
		else
			shiftsonsright(f);
		resize();
	} else if (r == Kdown || r == Kright){
		if (intagy(p, lastxy))
			incwin(f);
		else
			shiftsonsleft(f);
		resize();
	}
}

Panel*
newpanel(char* name, Panel* parent)
{
	Panel*	p;
	int	id;

	id = paneltype(name);
	if (id < 0){
		werrstr("bad panel type");
		return nil;
	}
	p = emalloc9p(sizeof(Panel));
	memset(p, 0, sizeof(Panel));
	p->ref = 1;
	p->name = estrdup9p(name);
	p->type = id;
	p->atime = time(nil);
	if (parent && parent->con){
		incref(parent->con);
		p->con = parent->con;
	}
	edprint("newpanel %p %s %d\n", p, p->name, p->type);
	panels[id]->init(p);
	return p;
}

void
closepanel(Panel* p)
{
	extern File* focus;

	if (p == nil)
		return;
	focus = nil; 
	p->flags |= Phide;	// prevent redraws (paranoia).
	edprint("closepanel %p %s %d\n", p, p->name, p->type);
	panels[p->type]->term(p);
	conprint(p->con, "%s exit\001", p->path);
	free(p->name);
	free(p->path);
	closecon(p->con);
	memset(p, 0, sizeof(*p));	// poison
	free(p);
}

int
hidden(File* f)
{
	Panel*	p;

	if (!f || !(f->qid.type&QTDIR) || !f->aux)
		return 1;
	p = f->aux;
	if (p->flags&Phide)
		return 1;
	if (Dx(p->rect) <= 0 || Dy(p->rect) <= 0)
		return 1;
	if (f->parent == f || f->parent == nil)
		return 0;
	else
		return hidden(f->parent);
}

void
rmhuppanels(File* f, Con* c)
{
	File**	w;
	File**	l;
	File*	fp;
	Panel*	p;
	Panel*	np;

	p = f->aux;
	if (p->con == c)
		rmpanel(f);
	else {
		w = newfilewalk(f);
		for (l = w; fp = *l; l++){
			np = fp->aux;
			if (np->con == c)
				rmpanel(fp);
			else if (np->type == Qcol || np->type == Qrow)
				rmhuppanels(fp, c);
		}
		endfilewalk(w);
	}
}

File*
pointinpanel(Point xy, int atomok)
{
	File*	f;
	File*	fp;
	Panel*	p;
	Panel* np;
	File**	w;
	File**	l;
	int	nloop;

	w = newfilewalk(slash);
	f = w[0];
	endfilewalk(w);
	p = f->aux;
	if (!ptinrect(xy, p->rect))
		return f;
	nloop = 0;
again:
	assert(nloop++ < 50);
	w = newfilewalk(f);
	for (l = w; fp = *l; l++){
		if (!ispanel(fp))
			continue;
		np = fp->aux;
		if (np->flags&Phide)
			continue;
		if (ptinrect(xy, np->rect))
		if (atomok || np->type == Qrow || np->type == Qcol){
			f = fp;
			endfilewalk(w);
			goto again;
		}
	}
	endfilewalk(w);
	return f;
}

int
hastag(File*f)
{
	Panel* p;

	p = f->aux;
	if (!strcmp(f->name, "/"))
		return 0;
	return (p->flags&Ptag);
}


File*
paneltop(File* f)
{
	Panel*	p;

	for(;;){
		p = f->aux;
		if (p->flags&Ptop)
			break;
		if (p->flags&Playout)
			break;
		if (f == f->parent)
			break;
		f = f->parent;
	}
	assert(f && f->aux == p);
	return f;
}


#include "/sys/src/lib9p/file.h"

/*
 * Called once after each new panel.
 * This minimizes all but the 2 MRU ones
 * within the same container.
 */
void
cleanpolicy(File* f)
{
	Panel*	p;
	File**	w;
	File**	l;
	Panel*	np;
	File*	f0;
	File*	f1;
	long	t0, t1;
	int	somewants;
	int	now;
	File*	tf;
	File*	fp;

	edprint("cleanpolicy for %s \n", f->name);

	// 1. Locate top container.

	tf = paneltop(f);
	if (tf == tf->parent || tf->parent == slash)
		return;	// no clean at / or /sysnameui
	else
		f = tf;
	p = f->aux;

	// 2. Locate the two MRU ones (clumsy way to do it)

	t0 = t1 = 0;
	f0 = f1 = nil;
	somewants = 0;
if (0 && f->filelist->f)
print("f %p ref %ld 1st %p ref %ld\n", f, f->ref, f->filelist->f, f->filelist->f->ref);
	w = newfilewalk(f);
	for (l = w; fp = *l; l++){
		if (!ispanel(fp))
			continue;
		np = fp->aux;
		edprint("\t %s\t%ld at %x flags %d wx %d wy\n",
			fp->name, np->atime, np->flags, np->wants.x, np->wants.y);
		if (np->flags&Phide)
			continue;
		if (np->type != Qcol && np->type != Qrow)
			continue;
		if (p->type == Qcol && np->wants.y)
			somewants++;
		if (p->type == Qrow && np->wants.x)
			somewants++;
		if (t0 < np->atime){
			t0 = np->atime;
			f0 = fp;
		}
	}
if (0 && f->filelist->f)
print("e %p %s ref %ld 1st %p ref %ld\n", f, f->name, f->ref, f->filelist->f, f->filelist->f->ref);
	endfilewalk(w);
if (0 && f->filelist->f)
print("w %p ref %ld 1st %p ref %ld\n", f, f->ref, f->filelist->f, f->filelist->f->ref);
	if (somewants < 1)
		return;
	w = newfilewalk(f);
	for (l = w; fp = *l; l++){
		if (!ispanel(fp))
			continue;
		np = fp->aux;
		if (np->flags&Phide)
			continue;
		if (np->type != Qcol && np->type != Qrow)
			continue;
		if (t1 < np->atime && f0 != fp){
			t1 = np->atime;
			f1 = fp;
		}
	}
	endfilewalk(w);

	// 3. Minimize other ones.

	edprint("=> f0 %s f1 %s\n", f0 ? f0->name : "nil", f1 ? f1->name : "nil");
	now = time(nil);
	w = newfilewalk(f);
	for (l = w; fp = *l; l++){
		if (!ispanel(fp))
			continue;
		np = fp->aux;
		if (np->flags&Phide)
			continue;
		if (now - np->atime < 10)
			continue;
		// Experiment: keep rows as they are.
		if (np->type != Qcol /* && np->type != Qrow */)
			continue;
		if (fp != f0 && fp != f1 && np->nwins != 1){
			edprint("\tminwin %s\n", fp->name);
			minwin(fp);
		} else
			edprint("\t nomin %s (%d nw)\n", fp->name, np->nwins);
	}
	endfilewalk(w);
}

int
genctl(Panel* p, char* op)
{
	Font*	f;
	char*	ea;
	Panel*	pfp;
	int	mustdraw;

	edprint("%s: ctl: %s\n", p->name, op);
	mustdraw = 0;
	if (!strcmp(op, "hide") && !(p->flags&Ptop)){
		if (!(p->flags&Phide)){
			p->flags |= Phide;
			pfp = p->file->parent->aux;
			pfp->flags |= Pmore;
			resize();
		}
	} else if (!strncmp(op, "show", 4)){
		if (p->flags&Phide){
			p->flags &= ~Phide;
			pfp = p->file->parent->aux;
			if (!hassons(p->file->parent, Phide))
				pfp->flags &= ~Pmore;
			resize();
		}
	} else if (!strncmp(op, "tag", 3) && !(p->flags&Playout)){
		if (!(p->flags&Ptag)){
			p->flags |= Ptag|Predraw;
			mustdraw = 1;
		}
	} else if (!strncmp(op, "notag", 5) && !(p->flags&Playout)){
		if (p->flags&Ptag){
			p->flags &= ~Ptag;
			p->flags |= Predraw;
			mustdraw = 1;
		}
	} else if (!strncmp(op, "dirty", 5)){
		if (!(p->flags&Pdirty)){
			p->flags |= Pdirty;
			borders(p->file->parent);
			mustdraw = 1;
		}
	} else if (!strncmp(op, "clean", 5)){
		if (p->flags&Pdirty){
			p->flags &= ~Pdirty;
			borders(p->file->parent);
			mustdraw = 1;
		}
	} else if (!strncmp(op, "font ", 5)){
		f = getfont(op+5);
		if (f && f != p->font){
			p->font = f;
			p->flags |= Predraw;
			mustdraw = 1;
		}
	} else if (!strncmp(op, "addr ", 5) && !(p->flags&Playout)){
		closecon(p->con);
		ea = strchr(op+5, ' ');
		if (ea) *ea = 0;
		p->con = newcon(op + 5);
		setctlfilelen(p);
		event(p, "addr /devs/%sui %s", sname, saddr);
	} else if (!strncmp(op, "size ", 5)){
		; /* accepted, but ignored. To allow "cat ctl >.../ctl" */
	} else if (!strcmp(op, "min")){
		if (p->type == Qcol || p->type == Qrow){
			p->nwins = 0;
			minwin(p->file);
			resize();
		}
	} else if (!strcmp(op, "nomin")){
		if (p->type == Qcol || p->type == Qrow){
			if (p->flags&Pmore){
				fullwin(p->file);
				resize();
			}
		}
	} else if (!strncmp(op, "space ", 6)){
		ea = strchr(op+6, ' ');
		if (ea) *ea = 0;
		p->space = atoi(op+6);
		p->flags |= Predraw;
		mustdraw = 1;
	} else {
		werrstr("bad ctl: %s", op);
		return -1;
	}
	return mustdraw;
}

void
setctlfilelen(Panel* p)
{
	long len;

	if (p->ctllen)
		len = p->ctllen;
	else
		len = 7 * 4;	// "xxx \n"  * #clts
	if (p->con)
		len += 5 + 30 + 1; // addr + address + \n
	p->cfile->length = len;
}

long
sprintattrs(Panel* p, char* str, long l, char* attr)
{
	char*	s;

	if (l <= 0)
		return -1;
	s = str;
	if (p->con)
		s = seprint(s, str+l, "addr %-30s\n", p->con->addr);
	if (p->flags&Ptag)
		s = seprint(s, str+l, "tag   \n");
	else
		s = seprint(s, str+l, "notag \n");
	if (p->flags&Phide)
		s = seprint(s, str+l, "hide  \n");
	else
		s = seprint(s, str+l, "show  \n");
	if (p->flags&Pdirty)
		s = seprint(s, str+l, "dirty \n");
	else
		s = seprint(s, str+l, "clean \n");
	if (p->font == fonts[FL])
		s = seprint(s, str+l, "font L\n");
	else if (p->font == fonts[FR])
		s = seprint(s, str+l, "font R\n");
	else if (p->font == fonts[FB])
		s = seprint(s, str+l, "font B\n");
	else
		s = seprint(s, str+l, "font T\n");
	if (attr)
		s = seprint(s, str+l, "%s", attr);
	return s - str;
}

long
genattrs(Panel* p, char* str, long l)
{
	return sprintattrs(p, str, l, nil);
}

int
genmouse(Panel* p, Cmouse* m, Channel* mc)
{
	if (m->buttons == 4){
		if (cookclick(m, mc))
			event(p, "look %11d %s", strlen(p->name), p->name);
		return 1;
	} else if (m->buttons == 2){
		if (cookclick(m, mc))
			event(p, "exec %11d %s", strlen(p->name), p->name);
		return 1;
	} else
		return 0;
}


void
showtree(File* f, int force)
{
	Panel*	p;
	File**	w;
	File**	l;
	File*	fp;

	if (f == nil)
		return;
	assert(f->aux);
	p = f->aux;
	panelok(p);
	force |= (p->flags&Predraw);
	p->flags &= ~Predraw;
	if (Dx(p->rect) <= 0 || Dy(p->rect) <= 0 || (p->flags&Phide))
		return;
	force |= (!eqrect(p->orect, p->rect));
	ldprint("show: %s\t%R %s\n", f->name, p->rect,
		(p->flags&Predraw) ? "show" : "dont");

	if (p->type == Qcol || p->type == Qrow){
		w = newfilewalk(f);
		for (l = w; fp = *l; l++)
			if (ispanel(fp))
				showtree(fp, force);
		endfilewalk(w);
		borders(f);
		return;
	}
	if (force){
		panels[p->type]->draw(p, 1);
		if (hastag(f))
			drawtag(p, 0);
	}
}
