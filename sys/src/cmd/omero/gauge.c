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

static void xinit(Panel*);
static void xterm(Panel*);
static int  xctl(Panel* p, char* ctl);
static long xread(Panel* p, void* buf, long cnt, vlong off);
static long xwrite(Panel* p, void* buf, long cnt, vlong off);
static void xmouse(Panel* p, Cmouse* m, Channel* mc);
static void xkeyboard(Panel*, Rune);
static void xdraw(Panel*, int);

Pops gaugeops = {
	.pref = "gauge:",
	.init = xinit,
	.term = xterm,
	.ctl = xctl,
	.attrs= genattrs,
	.read = xread,
	.write= xwrite,
	.draw = xdraw,
	.mouse = xmouse,
	.keyboard = genkeyboard,
};

Pops sliderops = {
	.pref = "slider:",
	.init = xinit,
	.term = xterm,
	.ctl = xctl,
	.attrs= genattrs,
	.read = xread,
	.write= xwrite,
	.draw = xdraw,
	.mouse = xmouse,
	.keyboard = genkeyboard,
};

static Image* setcol;
static Image* clearcol;

static void 
xinit(Panel* p)
{
	if (!strncmp(p->name, "slider:", 7))
		p->flags |= Pedit;
	p->minsz = Pt(120,20);
	clearcol = cols[HIGH];
	if (setcol == nil)
		setcol = allocimage(display, Rect(0,0,1,1), RGB24, 1, 0x5555AAFF);
}

static void
xterm(Panel*)
{
}


static void
xdraw(Panel* p, int )
{
	Rectangle set;
	Rectangle clear;
	Rectangle line;
	int	dx;
	int	dy;

	if (hidden(p->file) || Dx(p->rect) <= 0 || Dy(p->rect) <= 0)
		return;
	set = p->rect;
	if (Dy(p->rect) > p->minsz.y*2){
		dy = Dy(p->rect) - p->minsz.y*2;
		set.min.y += dy/2;
		set.max.y -= dy/2;
	}
	clear = set;
	line  = set;
	p->grect = set;
	dx = Dx(set) * p->pcent / 100;
	clear.min.x = set.max.x = line.max.x = set.min.x + dx;
	line.min.x = line.max.x++ - 1;
	draw(screen, set, setcol, nil, ZP);
	draw(screen, clear,  clearcol, nil, ZP);
	draw(screen, line,  display->black, nil, ZP);
	border(screen, p->grect, 1, cols[BORD], ZP);
	if ((p->flags&Ptag))
		drawtag(p, 0);
}

static void
xmouse(Panel* p, Cmouse* m, Channel* mc)
{
	Point	d;
	int	dx;
	int	nval;
	int	dt;
	int 	msec;

	if (genmouse(p, m, mc))
		return;
	if ((p->flags&Pedit) && m->buttons == 1){
		msec = 0;
		do {
			p->dfile->length = 12;
			if (m->xy.x < p->grect.min.x)
				m->xy.x = p->grect.min.x;
			if (m->xy.x > p->grect.max.x)
				m->xy.x = p->grect.max.x;
			d = subpt(m->xy, p->grect.min);
			dx = Dx(p->grect);
			nval = d.x * 100 / dx;
			dt = m->msec - msec;
			if (!msec || dt > 250)
				event(p, "data %d", nval);
			msec = m->msec;
			if (nval != p->pcent){
				p->pcent = nval;
				xdraw(p,0);
				flushimage(display, 1);
			}
			recv(mc, m);
		} while(m->buttons&1);
		while(m->buttons != 0)
			recv(mc, m);
	}
}

static long
xread(Panel* p, void* buf, long count, vlong off)
{
	char	str[20];

	seprint(str, str+sizeof(str), "%11d\n", p->pcent);
	return genreadbuf(buf, count, off, str, strlen(str));
}

static long
xwrite(Panel* p, void* buf, long cnt, vlong )
{
	char	str[20];

	if (cnt > sizeof(str)-1)
		cnt = sizeof(str)-1;

	strncpy(str, buf, cnt);
	str[cnt] = 0;
	p->pcent = atoi(str);
	if (p->pcent > 100)
		p->pcent = 100;
	if (p->pcent < 0)
		p->pcent = 0;
	if (!hidden(p->file)){
		xdraw(p, 0);
		putscreen();
	}
	p->dfile->length = 12;
	return cnt;
}

static int
xctl(Panel* p, char* ctl)
{
	int	r;

	r = genctl(p, ctl);
	if (r > 0)
		xdraw(p, 0);
	return r;
}
