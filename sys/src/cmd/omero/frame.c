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
#include <ctype.h>
#include "gui.h"

int framedebug;

void
framedump(Panel* p)
{
	if (!framedebug)
		return;
	print("froff %d chr %d ln %d max %d, llf %d full %d\n",
		p->froff, p->f.nchars, p->f.nlines,
		p->f.maxlines, p->f.lastlinefull, p->f.lastlinefull);
}

/*
 * The routines below update both the Tblock and the Frame,
 * they do nothing else. No events, no resizes, no flushimages.
 */

void
fillframe(Panel* p)
{
	Tblock*	b;
	int	n;
	int	pos;
	Point	pt;
	Rectangle	r;
	Rune	nl[1];

	nl[0] = L'\n';
	if (p->fimage == nil || p->f.lastlinefull)
		return;

	b = blockseek(p->blks, &n, p->froff + p->f.nchars);
	
	for(; b && !p->f.lastlinefull; b = b->next){
		pos = p->f.nchars;
		if (b->nr){
			fdprint("fill at %d+%d with ", p->froff, b->nr);
			fdprint("%d runes: [%.*S]\n", b->nr,
				(b->nr < 100 ? b->nr : 100), b->r + n);
			frinsert(&p->f, b->r+n, b->r+b->nr, pos);
		}
		n = 0;
	}
	pt = frptofchar(&p->f, p->f.nchars+1);
	if (pt.x > 0)
		pt.y += p->font->height;
	r = p->fimage->r;
	r.min.y = pt.y;
	draw(p->fimage, r, cols[BACK], nil, ZP);
	r = p->fimage->r;
	
}

static void
newsel(Frame* f, int p0, int p1, int showtick)
{
	int	e;

	if (showtick)
		frdrawsel(f, frptofchar(f, f->p0), f->p0, f->p1, 0);
	if (!f->lastlinefull || !f->nchars)
		e = f->nchars;
	else
		e = f->nchars - 1;
	if (p0 > e)
		p0 = e;
	if (p1 > e)
		p1 = e;
	if (p0 < 0)
		p0 = 0;
	if (p1 < 0)
		p1 = 0;
	f->p0 = p0;
	f->p1 = p1;
	if (showtick)
		frdrawsel(f, frptofchar(f, f->p0), f->p0, f->p1, 1);
}

void
reloadframe(Panel* p, int resized)
{
	Rectangle r;

	if (p->fimage == nil)
		return;
	assert(p->froff >= 0);
	assert(p->blks != nil);
	fdprint("text frame %p loaded: r %R\n", p, p->rect);
	r = Rpt(ZP, Pt(Dx(p->rect), Dy(p->rect)));
	frclear(&p->f, resized);
	if (resized){
		freeimage(p->fimage);
		p->fimage = allocimage(display, r, screen->chan, 0, CBack);
	}
	frinit(&p->f, r, p->font, p->fimage, cols);
	fillframe(p);
	newsel(&p->f, p->ss - p->froff, p->se - p->froff, (p->flags&Pedit));
}

void
setframefont(Panel* p, Font* f)
{
	if (f != p->font){
		p->font = f;
		reloadframe(p, 0);
	}
}

/* Returns 0 if size did not change.
 * Returns 1 if #rows/#cols change
 * Returns 2 if we want a resize
 */
int
setframesizes(Panel* p)
{
	int	oncols, onrows;
	int	res;
	Point	omin;
	int	n;

	res = p->maxsz.y = 0;
	if (p->flags&Pline){
		omin = p->minsz;
		packblock(p->blks);
		p->minsz.y = fontheight(p->font);
		if (!(p->flags&Pedit) && p->blks->nr > 0){
			n = runestringnwidth(p->font, p->blks->r, p->blks->nr);
			p->minsz.x = n;
		}
		res = !eqpt(omin, p->minsz);
	} else {
		p->minsz = Pt(p->font->height*2,p->font->height*2);
		if (p->nlines == 0)
			p->maxsz.y = 0;
		else {
			n = p->nlines + 1;
			if (n < 3)
				n = 3;
			p->maxsz.y =  p->font->height * n;
		}
	}
	oncols = p->ncols;
	onrows = p->nrows;
	p->ncols = gettextwid(p);
	p->nrows = gettextht(p);
	fdprint("setframesizes: %s: sz %P min %P max [0 %d] txt %dx%d wx %d wy %d\n",
		p->name, p->size, p->minsz, p->maxsz.y, p->ncols, p->nrows,
		p->wants.x, p->wants.y);
	if (!res)
		res =  oncols != p->ncols || onrows != p->nrows;
	return res;
}

void
setframesel(Panel* p, int ss, int se, int setmpos)
{
	int	sw;

	if (ss == se){
		p->sdir = 0;
		p->s0 = ss;
	}
	if (ss > se){
		sw = ss;
		ss = se;
		se = sw;
	}
	if (setmpos)
		p->mpos = p->s0;
	if (ss < p->s0)
		p->sdir = -1;
	if (se > p->s0)
		p->sdir = 1;
	p->ss = ss;
	p->se = se;
	if (p->fimage != nil)
		newsel(&p->f, p->ss-p->froff, p->se-p->froff, (p->flags&Pedit));
}

void
addframesel(Panel* p, int pos)
{
	setframesel(p, p->s0, pos, 0);
}


void
jumpframepos(Panel* p, int pos)
{
	if (p->fimage)
	if (pos < p->froff || pos >= p->froff + p->f.nchars){
		p->froff = pos - 10;
		if (p->froff < 0)
			p->froff = 0;
		else if (p->froff > 0)
			scrollframe(p, -1);
		else
			reloadframe(p, 0);
	}
}

void
jumpframesel(Panel* p)
{
	jumpframepos(p, p->ss);
}

void
setframemark(Panel* p, int pos)
{
	int	l;

	l = blocklen(p->blks);
	p->mark = pos;
	if (p->mark < 0)
		p->mark = 0;
	if (p->mark > l)
		p->mark = l;
}

int
framehassel(Panel* p)
{
	return p->sdir;
}

int
posselcmp(Panel* p, int pos)
{
	if (pos < p->ss)
		return -1;
	if (pos >= p->se)
		return 1;
	return 0;
}


int
findln(Tblock* b, int* pp)
{
	int	i;
	int	pos;

	pos = *pp;
	for(i = 0; i < 128 && pos < b->nr; i++, pos++)
		if (b->r[pos] == '\n')
			break;
	*pp = pos;
	return (i == 128 || b->r[pos] == '\n');
}

int
findrln(Tblock* b, int* pp)
{
	int	i;
	int	pos;

	pos = *pp;
	for(i = 0; i < 128 && pos > 0; i++, pos--)
		if (b->r[pos] == '\n')
			break;
	*pp = pos;
	return (i == 128 || b->r[pos] == '\n' || pos == 0);
}

int
scrollframe(Panel* p, int nscroll)
{
	Tblock*	b;
	int	n;
	int	pos;
	int	nlines;
	int	l;

	fdprint("scroll %d\n", nscroll);
	nlines = abs(nscroll);

	assert(p->froff >= 0);
	if (nscroll == 0)
		return 0;
	packblock(p->blks);
	b = p->blks;
	if (nscroll > 0) {
		l = blocklen(b);
		if (p->froff + p->f.nchars >= l)
			return 0;
		n = p->froff;
		while(nlines-- && findln(b, &n))
			if (n < b->nr)
				n++;
	} else {
		if (p->froff <= 0){
			if (blockdebug)
				blockdump(b);
			return 0;
		}
		n = p->froff;
		while(nlines-- && findrln(b, &n))
			if (n > 0)
				n--;
		if (n > 0 && n < b->nr) // advance to skip last \n
			n++;
	}
	if (!b)
		return 0;
	pos = n;
	if (b->r[pos] == '\n')
		pos++;
	frclear(&p->f, 0);
	p->froff = pos;
	reloadframe(p, 0);

	newsel(&p->f, p->ss - p->froff, p->se - p->froff, (p->flags&Pedit));
	framedump(p);
	return 1;
}


int
frameins(Panel* p, Rune* r, int nr, int pos)
{
	Tblock*	b;
	int	n;
	int	old;
	int	full;

	assert(pos <= blocklen(p->blks));
	fdprint("ins %d (p0 %uld) %d runes [%.*S]\n", pos, p->f.p0, nr, nr, r);
	b = blockseek(p->blks, &n, pos);
	blockins(b, n, r, nr);
	p->dfile->length += runenlen(r, nr);

	full = 0;
	if (p->fimage != nil)
		if (pos >= p->froff){
			old = p->f.nchars;
			frinsert(&p->f, r, r +nr, pos - p->froff);
			fdprint("old %d nchars %d\n", old, p->f.nchars);
			if (old == p->f.nchars)
				full = 1;
		} else
			p->froff += nr;
	if (pos < p->ss)
		p->ss += nr;
	if (pos < p->se)
		p->se += nr;
	if (pos < p->s0)
		p->s0 += nr;
	if (pos < p->mark)
		p->mark += nr;
	return full;
}

static int
fixpos(int pos, int x, int n)
{
	if (x < pos){
		if (x + n > pos)
			n = pos - x;
		pos -= n;
	}
	return pos;
}

int
framedel(Panel* p, Rune* r, int nr, int pos)
{
	Tblock*	b;
	int	n;
	Rune	nl[1];

	nl[0] = L'\n';
	assert(r);
	b = blockseek(p->blks, &n, pos);
	if (b == nil)
		return 0;
	blockdel(b, n, nr, r);
	p->dfile->length -= runenlen(r, nr);
	if (pos >= p->froff){
		if (p->fimage != nil){
			frdelete(&p->f, pos - p->froff, pos - p->froff + nr);
			/* Deleting on the last line leaves part of the tick
			 * in the screen. The rune is blanked but not the tick.
			 * We add a fake \n for a while just to clear the line.
			 * To me, it seems that it should be libframe the one
			 * clearing the right part of the last line. BUG?
			 */
			if (!p->f.lastlinefull){
				frinsert(&p->f, nl, nl+1, p->f.nchars);
				frdelete(&p->f, p->f.nchars-1, p->f.nchars);
			}
		}
	} else
		p->froff = fixpos(p->froff, pos, nr);
	// BUG: cut with a mark in the middle;
	p->ss = fixpos(p->ss, pos, nr);
	p->se = fixpos(p->se, pos, nr);
	p->s0 = fixpos(p->s0, pos, nr);
	p->mark = fixpos(p->mark, pos, nr);
	return nr;
}

static int
iswordchar(Rune r)
{
	return isalpharune(r) || runestrchr(L"0123456789|&?=._-+/:", r);
}

static Rune lparen[] = L"{[(«<“";
static Rune rparen[] = L"}])»>”";
static Rune paren[] = L"\"'`";

static int
isparen(Rune* set, Rune r)
{
	Rune* p;
	p = runestrchr(set, r);
	if (p)
		return p - set + 1;
	else
		return 0;
}

static int
findexpsep(void* a, int i, Rune r)
{
	int*	inword = a;

	//fprint(2, "[%d %C] ", i, r);
	if (*inword)
		return !iswordchar(r);
	else
		return (i > 128 || (int)r == '\n');
}

/* Returns the word at pos.
 * The word is the selection when it exists.
 * It is the longest set of <wordchar>s if pos at <wordchar>
 * It is the text between {} [] '' "" () if pos is at delim.
 * It is the current line otherwise (if pos at blank)
 */
Rune* 
framegetword(Panel* p, int pos, int* ss, int* se)
{
	Rune*	r;
	Tblock*	b;
	int	spos, epos;
	int	nr;
	int	pi;
	int	nparen;

	b = p->blks;
	packblock(b);
	assert(pos <= b->nr);
	spos = epos = pos;
	if (b->nr > 0)
	if (framehassel(p) && !posselcmp(p, pos)){
		spos = p->ss;
		epos = p->se;
	} else if (iswordchar(b->r[pos])){
		while(spos > 0 && iswordchar(b->r[spos]))
			spos--;
		if (spos > 0)
			spos++;
		while(epos < b->nr && iswordchar(b->r[epos]))
			epos++;
	} else if (pi = isparen(paren, b->r[pos])){
		spos++;
		for(epos = spos; epos < b->nr; epos++)
			if (isparen(paren, b->r[epos]) == pi)
				break;
	} else if (pi = isparen(lparen, b->r[pos])){
		nparen = 1;
		spos++;
		for(epos = spos; epos < b->nr; epos++){
			if (isparen(lparen, b->r[epos]) == pi)
				nparen++;
			if (isparen(rparen, b->r[epos]) == pi)
				nparen--;
			if (nparen <= 0){
				break;
			}
		}
	} else if (pi = isparen(rparen, b->r[pos])){
		nparen = 1;
		if (spos > 0)
		for(spos--; spos > 0; spos--){
			if (isparen(rparen, b->r[spos]) == pi)
				nparen++;
			if (isparen(lparen, b->r[spos]) == pi)
				nparen--;
			if (nparen <= 0){
				spos++;
				break;
			}
		}
	} else { // pos at blank
		if (b->r[spos] == '\n' && spos > 0 && b->r[spos-1] != '\n'){
			// click at right part of line; step back
			// so that expanding leads to previous line
			spos--;
		}
		while(spos > 0 && b->r[spos-1] != '\n')
			spos--;
		while(epos < b->nr && b->r[epos] != '\n')
			epos++;
		if (epos < b->nr)
			epos++;	// include \n
	}
	if (epos > b->nr)
		fprint(2, "epos bug: %d %d %d\n", spos, epos, b->nr);
	assert(spos >= 0);
	assert(epos >= 0);
	assert(epos >= spos);
	assert(epos <= b->nr);
	if (ss){
		*ss = spos;
		*se = epos;
	}
	nr = epos - spos;
	r = emalloc9p((nr  + 1) * sizeof(Rune));
	if (nr > 0)
		blockget(b, spos, nr, r);
	r[nr] = 0;
	edprint("framegetword %.*S %d %d\n", nr, r, spos, epos);
	return r;
}
