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

/*
 * Would improve the usage of memory and the time if we
 * keep the last N images loaded and use them as a cache.
 */

static void xinit(Panel*);
static void xterm(Panel*);
static int  xctl(Panel* p, char* ctl);
static long xattrs(Panel* p, char* buf, long sz);
static long xread(Panel* p, void* buf, long cnt, vlong off);
static long xwrite(Panel* p, void* buf, long cnt, vlong off);
static void xmouse(Panel* p, Cmouse* m, Channel* mc);
static long xwriteall(Panel* p, void* buf, long cnt);
static void xdraw(Panel* p, int );

Pops imageops = {
	.pref = "image:",
	.init = xinit,
	.term = xterm,
	.ctl = genctl,
	.attrs= xattrs,
	.read = xread,
	.write= xwrite,
	.draw = xdraw,
	.mouse = xmouse,
	.keyboard = genkeyboard,
	.writeall = xwriteall,
};

Pops pageops = {
	.pref = "page:",
	.init = xinit,
	.term = xterm,
	.ctl = genctl,
	.attrs= xattrs,
	.read = xread,
	.write= xwrite,
	.draw = xdraw,
	.mouse = xmouse,
	.keyboard = genkeyboard,
	.writeall = xwriteall,
};

static void 
xinit(Panel* p)
{
	p->minsz = Pt(48,48);
	p->flags |= Pedit;
	p->ctllen = 7 * 4 	+ 5 + 11 + 1 + 11 + 1;
	if (!strncmp(p->name, "page:", 5))
		p->wants = Pt(1,1);

	p->irect = Rect(0, 0, p->minsz.x, p->minsz.y);
}

static void 
xterm(Panel* p)
{
	free(p->idata);
	if (p->image)
		freeimage(p->image);
}


static void
xdraw(Panel* p, int )
{
	Point	pt;

	if (hidden(p->file) || Dx(p->rect) <= 0 || Dy(p->rect) <= 0)
		return;
	if (p->image){
		if (p->wants.x)
			pt = Pt(p->hoff,p->voff);
		else
			pt = ZP;
		pt = addpt(p->image->r.min, pt);
		if (p->wants.x)
			draw(screen, p->rect, cols[BACK], nil, ZP);
		draw(screen, p->rect, p->image, cols[BACK], pt);
	}
	if (p->flags&Ptag)
		drawtag(p, 0);
}

static long
xread(Panel* p, void* buf, long cnt, vlong off)
{
	return genreadbuf(buf, cnt, off, p->idata, p->isize);
}

static long
xwrite(Panel* , void* , long , vlong )
{
	// We have writeall. No write should reach us.
	fprint(2, "xwrite called for image\n");
	abort();
	return -1;
}

/*
 * Kludge: There's a bug I did not want to
 * track in libdraw, and readimage was the only
 * generic way I could find out to load an
 * image. This lock protects the file used to read
 * such an image.
 */
static QLock imglck;

static long
xwriteall(Panel* p, void* buf, long cnt)
{
	Image*	nimage;
	int	fd;
	Rectangle old;

	if (cnt <= 0){
		edprint("bad count to xwriteall\n");
		return -1;
	}
	/* BUG: using readimage is the only way that seems
	 * to work for all images. Must undo this kludge and
	 * track what happens. No time for that now.
	 */
	qlock(&imglck);
	fd = create("/tmp/omero.image", ORDWR|ORCLOSE, 0644);
	if (fd < 0){
		edprint("image: %r\n");
		qunlock(&imglck);
		return 0;
	}
	free(p->idata);
	p->idata = emalloc9p(cnt);
	memmove(p->idata, buf, cnt);
	p->isize = cnt;
	p->dfile->length = p->isize;
	write(fd, p->idata, p->isize);
	seek(fd, 0, 0);
	nimage = readimage(display, fd, 0);
	close(fd);
	qunlock(&imglck);
	if (nimage == 0){
		edprint("load image failed: %r\n");
		return -1;
	} else {
		memset(&old, 0, sizeof(old));
		if (p->image){
			old = p->image->r;
			freeimage(p->image);
		}
		p->image = nimage;
		p->voff = 0;
		if (!p->wants.x){
			p->minsz.x = Dx(p->image->r);
			p->minsz.y = Dy(p->image->r);
		}
		if (eqrect(old, p->image->r)){
			xdraw(p, 0);
			putscreen();
		} else
			resize();
		return cnt;
	}
}

static long
xattrs(Panel* p, char* str, long l)
{
	char	size[40];

	seprint(size, size+sizeof(size),
		"size %11d %11d\n", Dx(p->rect), Dy(p->rect));
	return sprintattrs(p, str, l, size);
}

static void
ijump(Panel* p, Point pt)
{
	int	dy, dx;
	int	diy, dix;

	if (pt.x < 0)
		pt.x = 0;
	if (pt.y < 0)
		pt.y = 0;
	dy = Dy(p->rect);
	diy= Dy(p->image->r);
	dx = Dx(p->rect);
	dix= Dx(p->image->r);
	p->voff = pt.y * diy / dy;
	p->hoff = pt.x * dix / dx;
	if (p->voff < 0)
		p->voff = 0;
	if (diy <= dy)
		p->voff = 0;
	else if (p->voff > diy - dy)
		p->voff = diy - dy ;
	if (p->hoff < 0)
		p->hoff = 0;
	if (dix <= dx)
		p->hoff = 0;
	else if (p->hoff > dix - dx)
		p->hoff = dix - dx;
}

static void
xmouse(Panel* p, Cmouse* m, Channel* mc)
{
	Point	xy;

	if (!p->wants.x){
		genmouse(p, m, mc);
		return;
	}
	if (m->buttons == 4){
		recv(mc, m);
		if (!m->buttons){
			event(p, "look %11d %s", strlen(p->name), p->name);
			return;
		}
		while(m->buttons & 4){
			xy = subpt(m->xy, p->rect.min);
			ijump(p, xy);
			xdraw(p, 0);
			flushimage(display, 1);
			recv(mc, m);
		}
		while(m->buttons)
			recv(mc, m);
	} else  if (m->buttons == 2){
		if (cookclick(m, mc))
			event(p, "exec %11d %s", strlen(p->name), p->name);
	}
}
