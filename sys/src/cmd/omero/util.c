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

/*
 * BUG: This touches the private parts of lib9p.
 *
 * I don't know how else to do it without
 * clobbering lib9p with stuff that does not belong there
 * The main reason for needing this is that omero uses the
 * File hierarchy to represent the widget hierarchy, and it's
 * just TOO dependent on the list of children. The main problems
 * are shifting left/right a set of panels and moving a panel
 * from one point in the tree to another. Any other stuff should
 * be using newfilewalk/endfilewalk without touching f->filelist.
 */
#include "/sys/src/lib9p/file.h"

File**
newfilewalk(File* f)
{
	File**	sons;
	File**	s;
	Filelist* l;

	wlock(f);
	s = sons = emalloc9p(sizeof(File*)*(f->nchild+1));
	sons[f->nchild] = nil;
	for (l = f->filelist; l != nil; l = l->link){
		if (l->f){
			incref(l->f);
			*s++ = l->f;
		}
	}
	wunlock(f);
	return sons;
}

void
endfilewalk(File** sons)
{
	File** s;

	for (s = sons; *s != nil; s++)
		closefile(*s);
	free(sons);
}

void
shiftsonsright(File* f)
{
	Filelist**fl;
	Filelist*fle;
	int	max;
	File*	ff;
	Panel*	ffp;

	wlock(f);
	max = 0;
again:
	for(fl=&f->filelist; (*fl) && (*fl)->link ; fl= &(*fl)->link)
		;
	if (fl && *fl && *fl != f->filelist){
		fle = *fl;
		*fl = nil;
		fle->link = f->filelist;
		f->filelist = fle;
	}
	ff = nil;
	ffp= nil;
	if (f->filelist){
		ff = f->filelist->f;
		if (ff)
			ffp = ff->aux;
	}
	if (max++ < 20)
	if (!ff || !(ff->qid.type&QTDIR) || (ffp->flags&Phide))
		goto again;
	wunlock(f);
}

void
shiftsonsleft(File* f)
{
	Filelist**fl;
	Filelist*fle;
	int	max;
	File*	ff;
	Panel*	ffp;

	wlock(f);
	max = 0;
again:
	for(fl=&f->filelist; (*fl) && (*fl)->link ; fl= &(*fl)->link)
		;
	if (fl && *fl && *fl != f->filelist){
		fle = *fl;
		fle->link = f->filelist;
		f->filelist = f->filelist->link;
		fle->link->link = nil;
	}

	ff = nil;
	ffp= nil;
	if (f->filelist){
		ff = f->filelist->f;
		if (ff)
			ffp = ff->aux;
	}
	if (max++ < 20)
	if (!ff || !(ff->qid.type&QTDIR) || (ffp->flags&Phide))
		goto again;
	wunlock(f);
}

void
drawtag(Panel* p, int setdirty)
{
	int	fl;
	Rectangle r, rr;

	fl = (p->flags&(Pmore|Pdirty));
	if (setdirty)
		fl |= Pdirty;
	r.min = p->rect.min;
	r.max = r.min;
	r.max.x += Tagwid;
	r.max.y += Taght;
	rr = r;
	rr.max.y += Taght;

	switch(fl){
	case 0:
		draw(screen, r, bord[Btag], nil, ZP);
		break;
	case Pmore:
		draw(screen, rr, bord[Bmtag], nil, ZP);
		break;
	case Pdirty:
		draw(screen, r, bord[Bdtag], nil, ZP);
		break;
	case Pmore|Pdirty:
		draw(screen, rr, bord[Bdmtag], nil, ZP);
		break;
	}
}

static void
drawspacetag(Panel* p)
{
	Rectangle r;
	int	space;

	space = p->space;
	if (space != 0){
		r.min.x = p->rect.min.x + Tagwid;
		r.max.x = p->rect.max.x - Inset;
		r.min.y = p->rect.min.y;
		r.max.y = p->rect.min.y + Inset;
		space--;
		space %= 3;
		draw(screen, r, bord[Bws1 + space], nil, ZP);
	}
}

void
borders(File* f)
{
	Panel* p;
	Panel* np;
	File**	w;
	File**	l;
	File*	fp;
	Rectangle r;
	Rectangle nr;
	Rectangle npr;
	int	has;
	int	last;
	int	dirty;
	Point	max;

	p = f->aux;
	if (hidden(f))
		return;
	assert(p->type == Qcol || p->type == Qrow);
	has = (p->flags&Ptag);
	dirty = (p->flags&Pdirty);
	if (Dx(p->rect) <= Inset || Dy(p->rect) <= Inset)
		return;
	r = insetrect(p->rect, Inset);
	if (has)
		r.min.x += Tagwid;
	max = ZP;
	last = 0;
	w = newfilewalk(f);
	for (l = w; fp = *l; l++){
		if (!ispanel(fp))
			continue;
		np = fp->aux;
		npr = np->rect;
		dirty |= (np->flags&Pdirty);
		if (!(np->flags&Phide)){
			// clear space unused by component.
			max = npr.max;
			if (p->type == Qcol){
				nr.min.x = npr.max.x;
				nr.max.x = r.max.x;
				nr.min.y = npr.min.y;
				nr.max.y = npr.max.y;
			} else {
				nr.min.y = npr.max.y;
				nr.max.y = r.max.y;
				nr.min.x = npr.min.x;
				nr.max.x = npr.max.x;
			}
			draw(screen, nr, cols[BACK], nil, ZP);
		}
		if (!(np->flags&Phide) && last != 0){
			// clear separator from previous component
			nr = r;
			if (p->type == Qcol){
				nr.min.y = last;
				nr.max.y = nr.min.y + Inset;
			} else {
				nr.min.x = last;
				nr.max.x = nr.min.x + Inset;
			}
			draw(screen, nr, cols[BACK], nil, ZP);
			last = 0;
		}
		if (!(np->flags&Phide))
			if (p->type == Qcol)
				last = np->rect.max.y;
			else
				last = np->rect.max.x;
	}
	endfilewalk(w);
	// Clear unused space at end of components
	if (max.x || max.y){
		nr = r;
		if (p->type == Qcol)
			nr.min.y = max.y;
		else
			nr.min.x = max.x;
		draw(screen, nr, cols[BACK], nil, ZP);
	} else {
		// no inner component. clear all space.
		draw(screen, r, cols[BACK], nil, ZP);
	}

	r = p->rect;
	r.max.x = r.min.x + Inset;
	if (has)
		r.max.x += Tagwid;
	r.min.y += Inset;
	r.max.y -= Inset;
	if (has)
		draw(screen, r, bord[Bw], nil, ZP);
	else
		draw(screen, r, bord[Bback], nil, ZP);

	r = p->rect;
	r.max.y = r.min.y + Inset;
	if (has)
		r.min.x += Tagwid;
	else
		r.min.x += Inset;
	r.max.x -= Inset;
	if (has)
		draw(screen, r, bord[Bn], nil, ZP);
	else
		draw(screen, r, bord[Bback], nil, ZP);

	r = p->rect;
	r.min.y = r.max.y - Inset;
	r.min.x += Inset;
	if (has)
		r.min.x += Tagwid;
	r.max.x -= Inset;
	if (has)
		draw(screen, r, bord[Bs], nil, ZP);
	else
		draw(screen, r, bord[Bback], nil, ZP);

	r = p->rect;
	r.min.x = r.max.x - Inset;
	r.min.y += Inset;
	r.max.y -= Inset;
	if (has)
		draw(screen, r, bord[Be], nil, ZP);
	else
		draw(screen, r, bord[Bback], nil, ZP);

	if (has)
		drawtag(p, dirty);

	r = p->rect;
	r.min.x = r.max.x - Inset;
	r.max.y = r.min.y + Inset;
	if (has)
		draw(screen, r, bord[Bne], nil, ZP);
	else
		draw(screen, r, bord[Bback], nil, ZP);
	
	r = p->rect;
	r.min.y = r.max.y - Inset;
	r.min.x = r.max.x - Inset;
	if (has)
		draw(screen, r, bord[Bse], nil, ZP);
	else
		draw(screen, r, bord[Bback], nil, ZP);
	
	r = p->rect;
	r.min.y = r.max.y - Inset;
	r.max.x = r.min.x + Inset;
	if (has)
		r.max.x += Tagwid;
	if (has)
		draw(screen, r, bord[Bsw], nil, ZP);
	else
		draw(screen, r, bord[Bback], nil, ZP);
	if (has)
		drawspacetag(p);
}

void
pstring(Point pt, Image* color, Font* font, char* s, Rectangle clipr)
{
	_string(screen, pt, color, ZP, font, s, nil, strlen(s), clipr,
			nil, ZP, SoverD);
}

int
flagsons(File* f, int set, int clr, int first, int last)
{
	File**	l;
	File**	w;
	File*	fp;
	Panel*	p;
	int	some;

	some = 0;
	w = newfilewalk(f);
	for (l = w; fp = *l; l++){
		if (ispanel(fp))
		if (--first < 0 && --last >= 0){
			p = fp->aux;
			p->flags |= set;
			p->flags &= ~clr;
			some++;
		}
	}
	endfilewalk(w);
	return some;
}

int
flagothersons(File* f, int set, int clr, File* excl)
{
	File**	l;
	File**	w;
	File*	fp;
	Panel*	p;
	int	some;

	some = 0;
	w = newfilewalk(f);
	for (l = w; fp = *l; l++){
		if (ispanel(fp))
		if (fp != excl){
			p = fp->aux;
			p->flags |= set;
			p->flags &= ~clr;
			some++;
		}
	}
	endfilewalk(w);
	return some;
}

int
hassons(File* f, int flag)
{
	File**	l;
	File**	w;
	File*	fp;
	Panel*	p;
	int	some;

	some = 0;
	w = newfilewalk(f);
	for (l = w; fp = *l; l++)
		if (ispanel(fp)){
			p = fp->aux;
			if (p->flags&flag)
				some++;
		}
	endfilewalk(w);
	return some;
}


void
detachfile(File* f)
{
	File *fp;
	Filelist *fl, **flp;
	
	fp = f->parent;
	assert(fp != nil && fp != f);

	wlock(f);
	wlock(fp);
	assert(f->parent == fp); //parent changed underfoot?

	for(flp=&fp->filelist; fl=*flp; flp=&(*flp)->link)
		if(fl->f == f){
			*flp = fl->link;
			break;
		}
	assert(fl != nil && fl->f == f);

	fl->f = nil;
	free(fl);
	fp->nchild--;
	f->parent = nil;
	wunlock(fp);
	wunlock(f);

	closefile(fp);	/* reference from child */
	closefile(f);	/* reference from tree */
	return;
}

void
insertfile(File* from, File* dir, File* after)
{
	Filelist *fl, *ne, *fle;

	wlock(from);
	wlock(dir);
	ne = emalloc9p(sizeof(*ne));
	ne->f = from;
	incref(from);
	ne->link = nil;
	from->parent = dir;
	incref(dir);
	dir->nchild++;
	if (dir->filelist == nil || after == nil){
		ne->link = dir->filelist;
		dir->filelist = ne;
		goto done;
	}
	fle = nil;
	for(fl=dir->filelist; fl; fl=fl->link){
		if(fl->f == after){
			ne->link = fl->link;
			fl->link = ne;
			goto done;
		}
		fle = fl;
	}
	assert(fle && !fle->link);
	fle->link = ne;
done:
	wunlock(dir);
	wunlock(from);
}

char*
filepath(File* f)
{
	int	i, l;
	char*	el[40];
	int	nel;
	char*	s;
	char*	sp;

	for (nel = 0; f && f != f->parent && nel < nelem(el); nel++){
		el[nel] = f->name;
		f = f->parent;
	}
	l = 2;
	for (i = 0; i <nel; i++)
		l += strlen(el[i]) + 1;
	s = sp = emalloc9p(l);
	*sp++ = '/';
	*sp = 0;
	for (i = nel - 1; i >= 0; i--){
		sp = strecpy(sp, s+l, el[i]);
		if (i > 0)
			*sp++ = '/';
	}
	return s;
}

void
event(Panel* p, char* fmt, ...)
{
	char*	ev;
	va_list	arg;

	if (p->con == nil || p->path == nil)
		return;
	va_start(arg, fmt);
	ev = vsmprint(fmt, arg);
	va_end(arg);
	conprint(p->con, "%s %s\001", p->path, ev);
	free(ev);
}

