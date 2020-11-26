#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <draw.h>
#include <mouse.h>
#include <keyboard.h>
#include <9p.h>
#include <frame.h>
#include "gui.h"
#include "cook.h"

typedef struct DCol DCol;

struct DCol {
	char*	name;
	int	col;
	Image*	img;
};

static void xinit(Panel*);
static void xterm(Panel*);
static long xattrs(Panel* p, char* buf, long sz);
static long xread(Panel* p, void* buf, long cnt, vlong off);
static long xwrite(Panel* p, void* buf, long cnt, vlong off);
static void xmouse(Panel* p, Cmouse* m, Channel* mc);
static void xdraw(Panel*, int);

Pops drawops = {
	.pref = "draw:",
	.init = xinit,
	.term = xterm,
	.ctl = genctl,
	.attrs= xattrs,
	.read = xread,
	.write= xwrite,
	.draw = xdraw,
	.mouse = xmouse,
	.keyboard = genkeyboard,
};


DCol dcols[] = {
	{"black", DBlack, nil},
	{"white", CBack, nil},
	{"grey", CClear, nil},
	{"green", DGreen, nil},
	{"red",	DRed, nil},
	{"blue", CSet, nil},
	{"cyan", DCyan, nil},
	{"magenta", DMagenta, nil},
	{"yellow", DYellow, nil},
};

static void
allocdcols(void)
{
	int	i;

	if (dcols[0].img != nil)
		return;

	for (i = 0; i < nelem(dcols); i++)
		dcols[i].img = allocimage(display, Rect(0,0,1,1), RGB24, 1, dcols[i].col);
}

static void 
xinit(Panel* p)
{
	allocdcols();
	p->minsz = Pt(48, 48);
	p->flags |= Pedit;
	p->ctllen = 7 * 4 + 5 + 11 + 1 + 11 + 1;
	p->irect = Rect(0, 0, p->minsz.x, p->minsz.y);
}

static void 
xterm(Panel* p)
{
	free(p->dcmds);
	if (p->canvas)
		freeimage(p->canvas);
}


static void
xdraw(Panel* p, int )
{
	if (hidden(p->file) || Dx(p->rect) <= 0 || Dy(p->rect) <= 0)
		return;
	if (p->canvas && p->rect.max.x)
		draw(screen, p->rect, p->canvas, nil, ZP);
	if (p->flags&Ptag)
		drawtag(p, 0);
}

static Point
atopt(char* s, char** ep)
{
	char*	e;
	Point	p;

	p.x = strtod(s, &e);
	p.y = strtod(e, &s);
	*ep = s;
	return p;
}

static Image*
atocol(char* s)
{
	int	i;

	if (s && *s == ' ')
		s++;
	if (!s || !*s)
		return nil;
	for (i = 0; i < nelem(dcols); i++)
		if (!strcmp(dcols[i].name, s))
			return dcols[i].img;
	return display->black;
}


static void
drawcmds(char* cmds, Image* canvas, Point* max)
{
	char*	s;
	char*	el;
	Rectangle	r;
	Point	pt;
	Point	rd;
	Image*	col;
	int thick;

	*max = ZP;
	for(s = cmds; s && *s;){
		el = strchr(s, '\n');
		if (el)
			*el++ = 0;
		if (strncmp(s, "ellipse ", 8)== 0){
			pt = atopt(s+7, &s);
			rd = atopt(s, &s);
			col = atocol(s);
			if (max->x < pt.x + rd.x)
				max->x = pt.x + rd.x;
			if (max->y < pt.y + rd.y)
				max->y = pt.y + rd.y;
			if (canvas && col)
				fillellipse(canvas, pt, rd.x, rd.y, col, ZP);
			if (canvas && !col)
				ellipse(canvas, pt, rd.x, rd.y, 0, cols[TEXT], ZP);
		} else if (strncmp(s, "rect ", 5)== 0){
			r.min = atopt(s+5, &s);
			r.max = atopt(s, &s);
			col = atocol(s);
			if (max->x < r.max.x)
				max->x = r.max.x;
			if (max->y < r.max.y)
				max->y = r.max.y;
			if (canvas)
				draw(canvas, r, col, nil, ZP);
		} else if (strncmp(s, "line ", 5) == 0){
			r.min = atopt(s+5, &s);
			r.max = atopt(s, &s);
			thick=strtod(s, &s);
			col = atocol(s);
			if (max->x < r.max.x)
				max->x = r.max.x;
			if (max->y < r.max.y)
				max->y = r.max.y;
			if (canvas)
				line(canvas, r.min, r.max, 0, 0, thick, col, ZP);
		} else if (strncmp(s, "poly ", 5) == 0){
			abort(); // BUG: poligon(Pt,Pt,Pt....)
		}
		s = el;
		if (el)
			*--el = '\n';
	}
	max->x++;
	max->y++;
	if (max->x < 10)
		max->x = 10;
	if (max->y < 10)
		max->y = 10;
}

static long
xattrs(Panel* p, char* str, long l)
{
	char	size[40];

	seprint(size, size+sizeof(size),
		"size %11d %11d\n", Dx(p->rect), Dy(p->rect));
	return sprintattrs(p, str, l, size);
}

static long
xread(Panel* p, void* buf, long count, vlong off)
{
	return genreadbuf(buf, count, off, p->dcmds, p->dsize);
}

static long
xwrite(Panel* p, void* buf, long cnt, vlong off)
{
	Image*	nimage;
	Rectangle r;
	Rectangle old;
	Point	max;

	if (off + cnt + 1> p->dsize){
		p->dcmds = erealloc9p(p->dcmds, off + cnt + 1);
		p->dsize = off + cnt;
	}
	memmove(p->dcmds + off, buf, cnt);
	p->dcmds[off + cnt] = 0;

	if (p->size.x > 0){
		p->dfile->length = strlen(p->dcmds);
		drawcmds(p->dcmds, nil, &max);
		r = Rpt(ZP, max);
		nimage = allocimage(display, r, screen->chan, 0, CBack);
		if (nimage == nil){
			edprint("allocimage: %r\n");
			return -1;
		}
		drawcmds(p->dcmds, nimage, &max);
		p->minsz = max;
		p->dloaded = 1;
		old = Rpt(ZP, ZP);
		if (p->canvas){
			old = p->canvas->r;
			freeimage(p->canvas);
		}
		p->canvas = nimage;
		if (!hidden(p->file)){
			if (eqrect(old, p->canvas->r)){
				xdraw(p, 0);
				putscreen();
			} else
				resize();
		}
	}

	return cnt;
}

static void
xmouse(Panel*p, Cmouse* m, Channel* mc)
{
	Point	pt;
	int	b;

	pt = m->xy;
	pt.x -= p->rect.min.x;
	pt.y -= p->rect.min.y;

	if ((b = m->buttons) && cookclick(m, mc))
		event(p, "click %11d %11d %11d %11d", pt.x, pt.y, b, m->msec);
}
