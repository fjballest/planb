#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <draw.h>
#include <mouse.h>
#include <ctype.h>
#include <keyboard.h>
#include <frame.h>
#include <9p.h>
#include <bio.h>
#include <b.h>
#include "gui.h"
#include "cook.h"

static void xinit(Panel*);
static void xterm(Panel*);
static int  xctl(Panel* p, char* ctl);
static long xattrs(Panel* p, char* buf, long sz);
static long xread(Panel* p, void* buf, long cnt, vlong off);
static long xwrite(Panel* p, void* buf, long cnt, vlong off);
static void xdraw(Panel* p, int resize);
static void xmouse(Panel* p, Cmouse* m, Channel* mc);
static void xkeyboard(Panel*, Rune);
static void xtruncate(Panel* p);

Pops textops = {
	.pref = "text:",
	.init = xinit,
	.term = xterm,
	.ctl = xctl,
	.attrs= xattrs,
	.read = xread,
	.write= xwrite,
	.draw = xdraw,
	.mouse = xmouse,
	.keyboard = xkeyboard,
	.truncate = xtruncate,
};

Pops tagops = {
	.pref = "tag:",
	.init = xinit,
	.term = xterm,
	.ctl = xctl,
	.attrs= xattrs,
	.read = xread,
	.write= xwrite,
	.draw = xdraw,
	.mouse = xmouse,
	.keyboard = xkeyboard,
	.truncate = xtruncate,
};

Pops labelops = {
	.pref = "label:",
	.init = xinit,
	.term = xterm,
	.ctl = xctl,
	.attrs= xattrs,
	.read = xread,
	.write= xwrite,
	.draw = xdraw,
	.mouse = xmouse,
	.keyboard = xkeyboard,
	.truncate = xtruncate,
};

Pops buttonops = {
	.pref = "button:",
	.init = xinit,
	.term = xterm,
	.ctl = xctl,
	.attrs= xattrs,
	.read = xread,
	.write= xwrite,
	.draw = xdraw,
	.mouse = xmouse,
	.keyboard = xkeyboard,
	.truncate = xtruncate,
};

static void 
xinit(Panel* p)
{
	char*	s;
	char*	q;
	int	l;

	if (!strncmp(p->name, "text:", 5)){
		p->flags |= Pedit;
		p->wants = Pt(1,1);
	} else if (!strncmp(p->name, "tag:", 4)){
		p->flags |= Pline|Pedit;
		p->wants = Pt(1,0);
	} else
		p->flags |= Pline;
	if (p->flags&Pline)
		p->font = fonts[FB];
	else
		p->font = fonts[FR];

	p->ctllen= 7 * 4  + 5 + 11 + 5 + 11 + 1 + 11 + 1 + 4 + 11 + 1 + 11 + 1 +1;

	if (p->flags&Pline){
		s = strchr(p->name, ':');
		q = strchr(s, '.');
		if (q)
			*q = 0;
		p->blks = str2block(s ? s+1 : p->name);
		if (q)
			*q = '.';
	} else
		p->blks = str2block("");
	assert(p->blks);
	if (p->flags&Pline)
		l = blocklen(p->blks);
	else
		l = 0;
	setframesel(p, l, l, 1);
	setframesizes(p);
}

static void 
xterm(Panel* p)
{
	free(p->snarf);
	blockfree(p->blks);
	cleanedits(p);
	if (p->fimage != nil){
		frclear(&p->f, 1);
		freeimage(p->fimage);
		p->fimage = nil;
	}
}

static int
dirty(Panel* p, int set, int sendev)
{
	int	old;

	if (p->flags&Pline)	// lines do not get dirty
		return 0;
	old = p->flags;
	if (set)
		p->flags |= Pdirty;
	else
		p->flags &= ~Pdirty;
	if (old != p->flags){
		borders(p->file->parent);
		if (sendev)
			event(p, set ? "dirty" : "clean" );
		return 1;
	}
	return 0;
}


static int
findnln(void* a, int , Rune r)
{
	int	c = (int)r;
	int	*np = a;

	if (c == '\n' && --(*np) < 0)
		return 1;
	return 0;
}

/* :xx search for a line number.
 * else, search for literal match.
 */
static int
search(Panel* p, char* key)
{
	Tblock*	b;
	int	pos;
	int	epos;
	int	ln;
	Rune*	r;
	Rune*	rp;

	b = p->blks;
	packblock(b);
	if (key[0] == ':' && isdigit(key[1])){
		ln = strtod(key+1, nil) - 1;
		if (ln < 0)
			ln = 0;
		pos = 0;
		while(pos < b->nr && ln > 0 && findln(b, &pos)){
			pos++;
			ln--;
		}
		epos = pos + 1;
		if (epos < b->nr && findln(b, &epos))
			epos++;
	} else {
		r = utf2runes(key, nil, nil);
		pos = p->ss + 1;
		if (pos < b->nr)
			rp = runestrstr(b->r + pos, r);
		else
			rp = nil;
		if (rp == nil){
			pos = 0;
			rp = runestrstr(b->r + pos, r);
		}
		if (rp == nil)
			epos = pos = 0;
		else {
			pos = rp - b->r;
			epos = pos + runestrlen(r);
		}
		free(r);
	}
	setframesel(p, pos, epos, 1);
	jumpframesel(p);
	return 1;
}

static void
xdraw(Panel* p, int resize)
{
	Rectangle	r;
	Point	pt;

	if (!p->file)
		return;

	/* Ugly. For buttons, we create the data from the
	 * file name. xinit does not have dfile to setup its
	 * length, and we must do that later.
	 */
	if (p->dfile && p->dfile->length == 0LL)
		p->dfile->length = blocksize(p->blks);
	
	if (hidden(p->file) || Dx(p->rect) < 10 || Dy(p->rect) < p->font->height)
		return;
	fdprint("text %s: redraw\n", p->name);
	if (p->fimage == nil){
		r = Rpt(ZP, Pt(Dx(p->rect), Dy(p->rect)));
		p->fimage = allocimage(display, r, screen->chan, 0, CBack);
		if (p->fimage == nil)
			sysfatal("xdraw: allocimage: %R: %r", r);
		frinit(&p->f, r, p->font, p->fimage, cols);
		setframesizes(p);
		resize = 0;
	} else if (resize)
		setframesizes(p);
	reloadframe(p, resize);
	r = p->rect;
	pt = subpt(screen->r.min,p->rect.min);
	draw(screen, screen->r, p->fimage, nil, pt);
	if (p->flags&Ptag)
		drawtag(p, 0);
}

static void
drawscrollbar(Panel* p)
{
	static Image* setcol;
	Rectangle r;
	Rectangle bar;
	int	y0, dy;
	int	ysz;
	int	l;

	if (setcol == nil)
		setcol = allocimage(display, Rect(0,0,1,1), RGB24, 1, 0x5555AAFF);
	bar.max.x = r.max.x = p->rect.max.x - Inset;
	bar.min.x = r.min.x = r.max.x - 15;
	r.min.y = p->rect.min.y + Inset;
	ysz = 90;
	r.max.y = r.min.y + ysz;
	if (r.max.y > p->rect.max.y){
		r.max.y = p->rect.max.y;
		ysz = Dy(r);
	}
	draw(screen, r, cols[HIGH], nil, ZP);
	l = p->dfile->length ? p->dfile->length : 1;
	y0 = ysz * p->froff / l;
	dy = ysz * p->f.nchars / l;
	if (dy < 3)
		dy = 3;
	bar.min.y = r.min.y + y0;
	bar.max.y = bar.min.y + dy;
	draw(screen, bar, setcol, nil, ZP);
	border(screen, r, 1, cols[BORD], ZP);
}


double
jumpscale(Panel* p, Point xy)
{
	double	dy;
	double	dc;

	dy = xy.y - p->rect.min.y;
	if (dy > p->rect.max.y - xy.y)
		dy = p->rect.max.y - xy.y;
	dc = blocklen(p->blks) - p->froff;
	if (dc < p->froff)
		dc = p->froff;
	if (dy < 1)
		dy = 1;
	return dc / dy;
}

static void
jump(Panel* p, int npos)
{
	Tblock*	b;
	int	n;

	if (abs(npos - p->froff) < 3)
		return;
	b = p->blks;
	packblock(b);
	if (npos > b->nr)
		npos = b->nr;
	if (npos > 0){
		n = npos;
		if (findrln(b, &n)){
			if (n > 0)
				n++;
			npos = n;
		}
	}
	if (npos < 0)
		npos = 0;
	p->froff = npos;
	reloadframe(p, 0);
}

static int
undocmd(Panel* p)
{
	if (undo(p)){
		dirty(p, hasedits(p), 1);
		xdraw(p, 0);
		jumpframesel(p);
		flushimage(display, 1);
		return 1;
	} else
		return 0;
}

static int
redocmd(Panel* p)
{
	if (redo(p)){
		dirty(p, hasedits(p), 1);
		xdraw(p, 0);
		jumpframesel(p);
		flushimage(display, 1);
		return 1;
	} else
		return 0;
}

static void
run(Panel* p, int pos, int onsel)
{
	Rune*	r;
	int	s, e;
	int	n;
	char*	c;

	r = framegetword(p, pos, &s, &e);
	c = smprint("%S", r);
	if (!wcommand(c)){
		n = e - s;
		event(p, "%s %11d %S", (onsel?"args":"exec"), runenlen(r, n), r);
	}
	free(c);
	free(r);
}

static void
look(Panel* p, int pos, int dclick)
{
	Rune*	r;
	int	s, e;
	int	n;
	char*	ev;

	r = framegetword(p, pos, &s, &e);
	n = e - s;
	if (dclick)
		ev = "Look";
	else
		ev = "look";
	if (n > 0)
		event(p, "%s %11d %S", ev, runenlen(r, n), r);
	free(r);
}

static int
cut(Panel* p, int putsnarf)
{
	char*	s;

	if (!framehassel(p))
		return 0;
	free(p->snarf);
	p->nsnarf = p->se - p->ss;
	p->snarf = malloc((p->nsnarf+1) * sizeof(Rune));
	if (p->nsnarf > 0){
		textdel(p, p->snarf, p->nsnarf, p->ss);
		addedit(p, Del, p->snarf, p->nsnarf, p->ss);
	} else
		p->snarf[0] = 0;
	fillframe(p);
	setframesel(p, p->ss, p->ss, 1);

	if (putsnarf){
		p->snarf[p->nsnarf] = 0;
		s = smprint("%S", p->snarf);
		writefstr("/dev/snarf", s);
		free(s);
	}
	return 1;
}

static int
nlines(Rune* r, int n)
{
	int	i, nl;
	int	lc;

	lc = nl = 0;
	for (i = 0; i < n; i++)
		if (r[i] == '\n' || ++lc > 80){
			nl++;
			lc = 0;
		}
			
	return nl;
}

int
textins(Panel* p, Rune* r, int nr, int pos)
{
	int	rc;

	rc = frameins(p, r, nr, pos);
	p->nlines += nlines(r, nr);
	event(p, "ins %11d %11d %.*S", pos, nr, nr, r);
	return rc;
}

void
textdel(Panel* p, Rune* r, int nr, int pos)
{
	if (nr > 0){
		nr = framedel(p, r, nr, pos);
		if (nr){
			p->nlines -= nlines(r, nr);
			event(p, "del %11d %11d", pos, nr);
		}
	}
}

static void
paste(Panel* p, int pos)
{
	Rune*	r;
	char*	s;

	if (s = readfstr("/dev/snarf")){
		r = utf2runes(s, nil, nil);
		free(s);
		free(p->snarf);
		p->snarf = r;
		p->nsnarf = runeslen(r);
		if (p->nsnarf){
			textins(p, p->snarf, p->nsnarf, pos);
			setframesel(p, pos, pos + p->nsnarf, 1);
			addedit(p, Ins, p->snarf, p->nsnarf, pos);
		}
	}
}

static void
mterm(Panel* p, Cmouse* m, Channel* mc)
{
	xdraw(p, 0);
	flushimage(display, 1);
	while(m->buttons){
		recv(mc, m);
		panelok(p);
	}
}

static void
mselcut(Panel* p, int)
{
	if (cut(p, 1))
		setframesizes(p);
	if (p->flags&Pedit){
		dirty(p, 1, 1);
	} else {
		undo(p);
		/* undo leaves the tick at the end,
		 * because ss != se, although beyond
		 * nchars. This removes the tick.
		 */
		setframesel(p, 0, 0, 0);
	}
}

static void
mselpaste(Panel* p, int pos)
{
	if (p->flags&Pedit){
		cut(p, 0);
		paste(p, pos);
		setframesizes(p);
		dirty(p, 1, 1);
	}
}

static void
writepsel(Panel* p)
{
	char*	s;

	if (p->flags&Pline)
		return;
	s = smprint("/devs%s", p->path);
	writefstr("/dev/sel", s);
	free(s);
}

static void
xmouse(Panel* p, Cmouse* m, Channel* mc)
{
	int	pos;
	Point	xy;
	Rune*	r;
	int	ss, se;
	int	updated;
	int	didcut, didpaste;
	double	jfactor;
	int	jpos, jy;

	panelok(p);
	if (p->fimage == nil || hidden(p->file))	// paranoia
		return;
	if (Dx(p->rect) <= 0 || Dy(p->rect) <= 0) // more paranoia
		return;
	xy = subpt(m->xy, p->rect.min);
	pos = p->froff + frcharofpt(&p->f, xy);

	if (m->buttons == 1){
		newedit(p);
		if (m->flag == CMdouble){
			r = framegetword(p, pos, &ss, &se);
			if (r){
				setframesel(p, ss, se, 1);
				free(r);
				writepsel(p);
			}
		} else
			setframesel(p, pos, pos, 1);
		updated = didcut = didpaste = 0;
		for(;;){
			xdraw(p, 0);
			flushimage(display, 1);
			recv(mc, m);
			panelok(p);
			if (m->buttons == 0)
				break;
			if (m->buttons == 1 && !didcut && !didpaste){	// slide
				xy = subpt(m->xy, p->rect.min);
				pos = p->froff + frcharofpt(&p->f, xy);
				addframesel(p, pos);
				if (p->ss != p->se && !updated++)
					writepsel(p);
			}
			if (m->buttons == 3){	// 1-2 chord
				if (didpaste){
					if (undo(p))
						dirty(p, hasedits(p), 0);
					mterm(p, m, mc);
					return;
				} else {
					didcut++;
					mselcut(p, p->ss);
				}
			}
			if (m->buttons == 5){	// 1-3 chord
				if (didcut){
					if (undo(p))
						dirty(p, hasedits(p), 0);
					mterm(p, m, mc);
					return;
				} else {
					if (!didpaste++)
						writepsel(p);
					mselpaste(p, p->ss);
				}
			}
		}
	} else if (m->buttons == 2){
		newedit(p);
		updated = 0;
		for(;;){
			recv(mc, m);
			panelok(p);
			if (m->buttons == 0)
				break;
			if (m->buttons == 2){	// slide
				if (!updated++)
					setframesel(p, pos, pos, 1);	
				xy = subpt(m->xy, p->rect.min);
				pos = p->froff + frcharofpt(&p->f, xy);
				addframesel(p, pos);
			}
			if (m->buttons == 3){	// 2-1 chord
				run(p, pos, 1);
				return;
			}
			xdraw(p, 0);
			flushimage(display, 1);
		}
		run(p, updated ? p->ss : pos, 0);
	} else if (m->buttons == 4){
		newedit(p);
		updated = 0;
		jfactor = 0;
		jy = m->xy.y;
		for(;;){
			recv(mc, m);
			panelok(p);
			if (m->buttons == 0)
				break;
			if (m->buttons == 4){	// slide
				updated++;
				if (jfactor == 0){
					pos = p->froff;
					jfactor = jumpscale(p, m->xy);
				}
				jpos = pos + (m->xy.y - jy) * jfactor;
				jump(p, jpos);
				xdraw(p, 0);
				drawscrollbar(p);
			}
			if (m->buttons == 5){	// 3-1 chord
				updated++;
				xy = subpt(m->xy, p->rect.min);
				pos = p->froff + frcharofpt(&p->f, xy);
				addframesel(p, pos);
			}
			flushimage(display, 1);
		}
		if (updated){
			xdraw(p, 0);
			flushimage(display, 1);
		} else
			look(p, pos, 0);
	}
}

static void
sendkeys(Panel* p, int ss, int se)
{
	Tblock*	b;
	int	n;
	Rune*	r;

	if (se - ss <= 0)
		return;
	b = blockseek(p->blks, &n, ss);
	r = emalloc9p((se - ss + 1) * sizeof(Rune));
	blockget(b, n, se-ss, r);
	event(p, "keys %S", r);
	free(r);
}

static void
xkeyboard(Panel* p, Rune r)
{
	int	k = r;
	Rune	buf[2];
	int	pos;

	if (p->fimage == nil || hidden(p->file))	// paranoia
		return;
	if (Dx(p->rect) <= 0 || Dy(p->rect) <= 0) // more paranoia
		return;
	pos = p->ss;
	switch(k){
	case Kbs:
		if (!(p->flags&Pedit)){
		kbevent:
			event(p, "keys %C", k);
			return;
		}
		if (pos - p->froff <= 0 && !scrollframe(p, -3)){
			fdprint("top\n");
			return;
		}
		if (!cut(p, 1)){
			if (pos)
				pos--;
			textdel(p, buf, 1, pos);
			setframesel(p, pos, pos, 0);
			addedit(p, Del, buf, 1, pos);
		}
	fill:
		dirty(p, 1, 1);
		fillframe(p);
		break;
	case 0x17:	// ^W. erase word
		if (!(p->flags&Pedit))
			goto kbevent;
		break;
	case 0x15:	// ^U. erase line
		if (!(p->flags&Pedit))
			goto kbevent;
		break;
	case Kdel:
		if (!(p->flags&Pedit))
			goto kbevent;
		if (!(p->flags&Pline)){
			event(p, "interrupt");
			return;
		}
		if (pos - p->froff >= p->f.nchars && !scrollframe(p, 3)){
			fdprint("bottom\n");
			return;
		}
		if (!cut(p, 1)){
			textdel(p, buf, 1, pos);
			addedit(p, Del, buf, 1, pos);
		}
		goto fill;
	case Kleft:
		undocmd(p);
		return;
	case Kright:
		redocmd(p);
		return;
	case Kup:
	case Kdown:
		if (p->flags&Pline)
			goto kbevent;
		else {
			scrollframe(p, (k == Kup) ? -3 : 3);
			reloadframe(p, 0);
		}
		break;
	case Kesc:
		if (!(p->flags&Pedit))
			goto kbevent;
		setframesel(p, p->mpos, pos, 0);
		break;
	case '\n':
		if (p->flags&Pline){
			setframesel(p, p->mpos, pos, 0);
			run(p, (pos>0 ? pos-1 : 0), 0);
			break;
		}
		if (p->flags&Pedit)
			event(p, "dirty");
		// and fall...
	default:
		if (!(p->flags&Pedit))
			goto kbevent;
		cut(p, 1);
		if (textins(p, &r, 1, pos))
			scrollframe(p, 3);
		setframesel(p, pos+1, pos+1, 0);
		addedit(p, Ins, &r, 1, pos);
		dirty(p, 1, 1);
		if (p->flags&Pedit)
			setframesizes(p);
	}
	xdraw(p, 0);
	flushimage(display, 1);
}

static long
xread(Panel* p, void* buf, long cnt, vlong off)
{
	char*	s;
	long	l;

	/* BUG: Need a real blockread here
	 */
	s = block2str(p->blks, &l);
	cnt = genreadbuf(buf, cnt, off, s, l);
	free(s);
	return cnt;
}

static long
xwrite(Panel* p, void* buf, long cnt, vlong off)
{
	int	pos;
	int	n;
	Rune*	r;
	int	nr;
	int	nout;
	Tblock*	b;
	Tblock*	nb;
	int	nl;
	char*	pb;

	/* BUG: Need a real blockwrite here
	 */
	assert(buf);
	pb = buf;
	if (off == 0LL)
		p->npartial = 0;
	if (p->npartial){
		if (p->partialoff == off){
			pb = emalloc9p(cnt + p->npartial);
			memmove(pb, p->partial, p->npartial);
			memmove(pb + p->npartial, buf, cnt);
		} else {
			p->npartial = 0;
			p->partialoff = 0;
			werrstr("partial rune");
			return -1;
		}
	}
	nout = p->npartial + cnt;
	r = utf2runes(pb, &nout, &nl);
	if (nout != p->npartial + cnt){
		n = p->npartial + cnt - nout;
		if (n >= UTFmax){	// binary file?
			p->npartial = 0;
			free(r);
			if (pb != buf)
				free(pb);
			werrstr("binary file?");
			return -1;
		}
		p->npartial = n;
		memmove(p->partial, pb + nout, p->npartial);
		p->partialoff = off + cnt;
	} else {
		p->npartial = 0;
		p->partialoff = 0;
	}
	if (pb != buf)
		free(pb);
	nr = runeslen(r);
	bdprint("data: %d runes [%.*S]\n", nr, nr, r);
	if (off == 0LL){
		pos = 0;
		blockfree(p->blks);
		p->blks = newblock(r, nr);
		p->nlines = nl;
	} else {
		pos = off2pos(p->blks, (long)off);
		b = blockseek(p->blks, &n, pos);
		b->nr = n;
		nb = newblock(r, nr);
		blockcat(b, nb);
		p->nlines += nl;
	}
	p->dfile->length = off+cnt;
	assert(pos + nr == blocklen(p->blks));
	if (p->flags&Pline)
		pos = pos + nr;
	else
		pos = 0;
	setframemark(p, pos);
	setframesel(p, pos, pos, 1);
	reloadframe(p, 0);

	cleanedits(p);
	if (setframesizes(p))
		resize();
	else if (p->fimage){
		xdraw(p, 0);
		putscreen();
	}
	return cnt;
}

static void
xtruncate(Panel* p)
{
	blockfree(p->blks);
	p->blks = newblock(nil, 0);
	p->nlines = 0;
	p->npartial = 0;
	p->mark = 0;
	p->dfile->length= 0LL;
	reloadframe(p, 0);
	setframemark(p, 0);
	setframesel(p, 0, 0, 1);
}


static int
textctlwr(Panel* p, char* s)
{
	char*	q;
	char*	r;
	Rune*	rs;
	int	ss, se;
	int	pos, n;
	int	nl;
	int	cnt;

	cnt = 1;
	if (!strncmp(s, "ins ", 4)){
		pos = strtod(s + 4, &q);
		n = 0;
		rs = utf2runes(s + 4 + 11 + 1 + 11 + 1, &n, &nl);
		if (n>0){
			frameins(p, rs, n, pos);
			addedit(p, Ins, rs, n, pos);
			p->nlines += nl;
			xdraw(p, 0);
			putscreen();
		}
		free(rs);
		return cnt;
	} else if (!strncmp(s, "del ", 4)){
		pos = strtod(s + 4, &q);
		n = strtod(q, &r);
		if (n>0){
			rs = emalloc9p(sizeof(Rune)*(n+1));
			framedel(p, rs, n, pos);
			addedit(p, Del, rs, n, pos);
			p->nlines -= nlines(rs, n);
			free(rs);
			xdraw(p, 0);
			putscreen();
		}
		return cnt;
	} else if (!strncmp(s, "sel ", 4)){
		if (!strncmp(s + 4, "all", 3)){
			ss = 0;
			se = blocklen(p->blks);
		} else {
			ss = strtod(s + 4, &q);
			se = strtod(q, &r);
			ss = off2pos(p->blks, ss);
			se = off2pos(p->blks, se);
		}
		setframesel(p, ss, se, 1);
		jumpframesel(p);
		xdraw(p, 0);
		putscreen();
		return cnt;
	} else if (!strncmp(s, "undo", 4)){
		if (!undocmd(p))
			cnt = 0;
		return cnt;
	} else if (!strncmp(s, "redo", 4)){
		if (!redocmd(p))
			cnt = 0;
		return cnt;
	} else if (!strncmp(s, "search ", 7)){
		search(p, s+7);
		xdraw(p, 0);
		putscreen();
		return cnt;
	} else if (!strcmp(s, "cut")){
		if (cut(p, 1)){
			setframesizes(p);
			dirty(p, 1, 1);
			xdraw(p, 0);
			putscreen();
		}
		return cnt;
	} else if (!strcmp(s, "paste")){
		cut(p, 0);
		paste(p, p->ss);
		setframesizes(p);
		xdraw(p, 0);
		dirty(p, 1, 1);
		putscreen();
		return cnt;
	} else if (!strncmp(s, "mark ", 5)){
		setframemark(p, pos2off(p->blks, atoi(s + 5)));
		return cnt;
	} else if (!strncmp(s, "exec ", 5)){
		if (!wcommand(s+5))
			event(p, "exec %11d %s", strlen(s+5), s+5);
		return cnt;
	} else if (!strncmp(s, "look ", 5)){
		event(p, "look %11d %s", strlen(s+5), s+5);
		return cnt;
	}
	return -1;
}

int
gettextwid(Panel* p)
{
	return Dx(p->rect) / stringwidth(p->font, "X");
}

int
gettextht(Panel* p)
{
	if (p->f.maxlines)
		return p->f.maxlines;
	else
		return Dy(p->rect) / fontheight(p->font);
}

static long
xattrs(Panel* p, char* str, long l)
{
	char	sel[80];

	seprint(sel, sel+sizeof(sel),
		"mark %11ld\nsel %11ld %11ld\nsize %11d %11d\n",
		pos2off(p->blks, p->mark),
		pos2off(p->blks, p->ss), pos2off(p->blks, p->se),
		gettextwid(p), gettextht(p));
	return sprintattrs(p, str, l, sel);
}

static int
xctl(Panel* p, char* ctl)
{
	int	r;
	int	odirty;

	odirty = p->flags&Pdirty;
	r = genctl(p, ctl);
	if (r < 0)
		r = textctlwr(p, ctl);
	if (r >= 0 && odirty && !(p->flags&Pdirty))
		setnoedits(p);
	if (r > 0){
		setframefont(p, p->font);
		if (!hidden(p->file) && Dx(p->rect) > 0 && Dy(p->rect) > 0)
		if ((p->flags&Pline) && setframesizes(p)){
			resize();
		} else {
			xdraw(p, 0);
			putscreen();
		}
	}
	return r;
}
