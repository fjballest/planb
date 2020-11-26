#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <draw.h>
#include <mouse.h>
#include <frame.h>
#include <9p.h>
#include "gui.h"

/* We follow the conventions in graphics(2).
 * Rectangles do not include the max point.
 */

int layoutdebug;

/*
 * Compute size and record if we want more x/y room
 * by looking into the inner components.
 * Save also current rect in orect.
 */
static void
size(File* f)
{
	Panel*	p;
	Panel*	np;
	File**	l;
	File**	w;
	File*	fp;
	int	some;

	p = f->aux;
	p->orect = p->rect;
	switch(p->type){
	case Qcol:		// lay out inner parts in rows
	case Qrow:		// lay out inner parts in cols
		if (hastag(f))
			p->size = Pt(Tagwid + Inset, Inset);
		else
			p->size = Pt(Inset, Inset);
		p->wants = Pt(0,0);
		some = 0;
		w = newfilewalk(f); 
		for (l = w; fp = *l; l++){
			if (!ispanel(fp))
				continue;
			np = fp->aux;
			if (np->flags&Phide)
				continue;
			some++;
			size(fp);
			// We ignore wants.[xy] != 1. See comment below.
			p->wants.x |= (np->wants.x == 1);
			p->wants.y |= (np->wants.y == 1);
			if (p->type == Qcol){
				if (some)
					p->size.y += Inset;
				p->size.y += np->size.y;
				if (p->size.x < np->size.x)
					p->size.x = np->size.x;
			} else {
				if (some)
					p->size.x += Inset;
				p->size.x += np->size.x;
				if (p->size.y < np->size.y)
					p->size.y = np->size.y;
			}
		}
		endfilewalk(w);
		if (!some && (p->flags&Playout))
			p->wants = Pt(1,1);
		addpt(p->size, Pt(Inset, Inset));
		break;
	default:
		p->size = p->minsz;
		if (p->size.x < Inset)	// ensure a min sz
			p->size.x = Inset;
		if (p->size.y < Inset)
			p->size.y = Inset;
		/* Heuristic to keep tiny text panels
		 * small. If they don't want too much,
		 * we assing the space and ignore their wants.
		 */
		if (p->wants.x)
			if (p->maxsz.x && p->maxsz.x < 120){
				p->wants.x = 2; // ignored later
				p->size.x = p->maxsz.x;
			} else
				p->wants.x = 1;
		if (p->wants.y)
			if (p->maxsz.y && p->maxsz.y < 120){
				p->wants.y = 2; // ignored later
				p->size.y = p->maxsz.y;
			} else
				p->wants.y = 1;
		break;
	}
}

/*
 * Recursively layout the hierarchy to its minimum size.
 * Panel.rect enters with the available rectangle
 * for showing the file and its hierarchy. It leaves
 * the routine with the actual rectangle used.
 */
static void
pack(File* f)
{
	Rectangle r;
	Point	max;
	Panel*	p;
	Panel* np;
	File**	w;
	File**	l;
	File*	fp;
	int	some;

	p = f->aux;
	switch(p->type){
	case Qcol:		// lay out inner parts in rows
	case Qrow:		// lay out inner parts in cols
		r = insetrect(p->rect, Inset);
		if (hastag(f))
			r.min.x += Tagwid;
		max = r.min;
		// r is always the avail rectangle.
		// max is the max. point used.
		some = 0;
		w = newfilewalk(f);
		for (l = w; fp = *l; l++){
			if (!ispanel(fp))
				continue;
			np = fp->aux;
			if (np->flags&Phide)
				continue;
			some++;
			np->rect = r;
			pack(fp);
			rectclip(&np->rect, p->rect);
			if (max.x < np->rect.max.x)
				max.x = np->rect.max.x;
			if (max.y < np->rect.max.y)
				max.y = np->rect.max.y;
			if (p->type == Qcol)
				r.min.y = np->rect.max.y + Inset;
			else
				r.min.x = np->rect.max.x + Inset;
		}
		endfilewalk(w);
		if (!some)
			goto atom;
		p->rect.max = addpt(max, Pt(Inset, Inset));
		break;

	default:	// Atom
	atom:
		if (p->size.x > Dx(p->rect))
			p->rect.max.x = p->rect.min.x + Dx(p->rect);
		else
			p->rect.max.x = p->rect.min.x + p->size.x;
		if (p->size.y > Dy(p->rect))
			p->rect.max.y = p->rect.min.y + Dy(p->rect);
		else
			p->rect.max.y = p->rect.min.y + p->size.y;
		break;
	}
	if(1)
	ldprint("pack %s: %03dx%03d %R\n", f->name,
		Dx(p->rect), Dy(p->rect), p->rect);
}

static void
move(File* f, Point pt)
{
	Panel*	p;
	File**	w;
	File**	l;
	File*	fp;
	Panel*	np;

	p = f->aux;
	p->rect.min.x += pt.x;
	p->rect.max.x += pt.x;
	p->rect.min.y += pt.y;
	p->rect.max.y += pt.y;
	w = newfilewalk(f);
	for (l = w; fp = *l; l++){
		if (!ispanel(fp))
			continue;
		np = fp->aux;
		if (!(np->flags&Phide))
			move(fp, pt);
	}
	endfilewalk(w);
}

/*
 * Expands inner components to use all the space
 * available in this one. Only those who want x/y space
 * are expanded.
 */
static void
expand(File* f)
{
	Panel*	p;
	Panel* np;
	File**	w;
	File**	l;
	File*	fp;
	int	n;
	int	nwx, nwy;
	Point	last;
	Point	spare;
	int	offset;
	int	incr;
	int	maxx, dx;

	p = f->aux;
	if (p->type != Qrow && p->type != Qcol)
		return;
	/*
	 * Determine space and how many want x,y room
	 */
	nwx = nwy = maxx = 0;
	last = ZP;
	w = newfilewalk(f);
	for (l = w; fp = *l; l++){
		if (!ispanel(fp))
			continue;
		np = fp->aux;
		if (!(np->flags&Phide)){
			dx = Dx(np->rect);
			if (dx > maxx)
				maxx = dx;
			if (np->wants.x == 1)
				nwx++;
			if (np->wants.y == 1)
				nwy++;
			last = np->rect.max;
		}
	}
	endfilewalk(w);
	spare = subpt(p->rect.max, last);
	spare = subpt(spare, Pt(Inset, Inset));

	/*
	 * Resize to consume spare space:
	 */

	/* 1. Try to make them equal sized.
	 *    By now, this is only done for rows.
	 *    Column processing would be equivalent.
	 */
	offset = 0;
	if (p->type == Qrow){
		w = newfilewalk(f);
		for (l = w; fp = *l; l++){
			if (!ispanel(fp))
				continue;
			np = fp->aux;
			if (np->flags&Phide)
				continue;
			if (p->type == Qrow){
				move(fp, Pt(offset, 0));
				dx = maxx - Dx(np->rect);
				if (np->wants.x == 1 && spare.x > 0 && dx > 0){
					incr = (dx > spare.x) ? spare.x : dx;
					np->rect.max.x += incr;
					offset += incr;
					spare.x -= incr;
				}
			}
			ldprint("expand.1: %s wx %d wy %d %dx%d %R\n", fp->name,
				np->wants.x, np->wants.y, Dx(np->rect),
				Dy(np->rect), np->rect);
			expand(fp);
		}
		endfilewalk(w);
	}
	/* 2. Proportional sharing of what remains
	 *    and extend the other coordinate to use whatever
	 *    empty space is there due to different sizes in that axys.
	 */
	offset = 0;
	w = newfilewalk(f);
	for (l = w; fp = *l; l++){
		if (!ispanel(fp))
			continue;
		np = fp->aux;
		if (np->flags&Phide)
			continue;
		if (p->type == Qcol){
			move(fp, Pt(0, offset));
			n = p->rect.max.x - Inset;
			if (np->rect.max.x < n)
			if (np->type == Qcol || np->type == Qrow || np->wants.x == 1)
				np->rect.max.x = n;
			if (np->wants.y == 1 && spare.y > 0){
				incr = spare.y/nwy;
				np->rect.max.y += incr;
				offset += incr;
			}
		} else {
			move(fp, Pt(offset, 0));
			n = p->rect.max.y - Inset;
			if (np->rect.max.y < n)
			if (np->type == Qcol || np->type == Qrow || np->wants.y == 1)
				np->rect.max.y = n;
			if (np->wants.x == 1 && spare.x > 0){
				incr = spare.x/nwx;
				np->rect.max.x += incr;
				offset += incr;
			}
		}
		ldprint("expand.2: %s wx %d wy %d %dx%d %R\n", fp->name,
			np->wants.x, np->wants.y, Dx(np->rect),
			Dy(np->rect), np->rect);
		expand(fp);
	}
	endfilewalk(w);
}

void
layout(File* f)
{
	Rectangle r;
	Panel*	p;

	p = f->aux;
	if (screen == nil)
		return;
	r = screen->r;
	p->rect = r;
	size(f);
	/* BUG: if none of /devs/ui/col:n wants.x/wants.y
	 * we should set wants.x/wants.y for any of them.
	 * Otherwise, the screen gets ugly.
	 */
	pack(f);
	p->rect = r;
	expand(f);
}
