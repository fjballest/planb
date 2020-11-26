#include <u.h>
#include <libc.h>
#include <draw.h>
#include <event.h>

typedef struct X10 X10;
struct X10 {
	char *name;
	char *file;
	Rectangle r;
	int on;
	int err;
	int locked;
};

X10 *x10;
int nx10;
Image *lightblue;
Image *yellow;
Image *red;
int	dsz=2;

enum {
	PAD = 3,
	MARGIN = 5
};

char *dir = "/devs/x10";

void*
erealloc(void *v, ulong n)
{
	v = realloc(v, n);
	if(v == nil)
		sysfatal("out of memory reallocating %lud", n);
	return v;
}

void*
emalloc(ulong n)
{
	void *v;

	v = malloc(n);
	if(v == nil)
		sysfatal("out of memory allocating %lud", n);
	memset(v, 0, n);
	return v;
}

char*
estrdup(char *s)
{
	int l;
	char *t;

	if (s == nil)
		return nil;
	l = strlen(s)+1;
	t = emalloc(l);
	memcpy(t, s, l);

	return t;
}

void
init(void)
{
	int i, j, fd, nr;
	Dir *pd;
	char buf[128];

	if((fd = open(dir, OREAD)) < 0)
		return;

	nr = dirreadall(fd, &pd);
	x10 = emalloc(nr * sizeof(X10));
	for(i=j=0; i<nr; i++){
		if (strcmp(pd[i].name,"cm11") == 0){
			continue;
		}
		sprint(buf, "%s/%s", dir, pd[i].name);
		x10[j].file = estrdup(buf);
		x10[j].name = estrdup(pd[i].name);
		x10[j].on = 0;
		nx10 = ++j;
	}
	free(pd);

	close(fd);
}

void
writex10(int i)
{
	int fd;
	char* str;
	int n;

	fd = open(x10[i].file, OWRITE);
	if (fd >= 0){
		str = ( x10[i].on ? "on" : "off");
		n = write(fd, str, strlen(str));
		if (n < 0)
			x10[i].err = 1;
	} else
		x10[i].err = 1;
	close(fd);
}

int
readx10(void)
{
	int i;
	int fd;
	char buf[80];
	int n;
	int changed;
	int last;

	changed = 0;
	for (i = 0; i < nx10; i++){
		last = x10[i].on;
		x10[i].on = 0;
		fd = open(x10[i].file, OREAD);
		n = read(fd, buf, sizeof(buf)-1);
		if (n > 0){
			buf[n] = 0;
			if (strcmp(buf, "on") == 0)
				x10[i].on = 1;
		}
		if (n < 0)
			x10[i].err = 1;
		close(fd);
		changed |= (x10[i].err || x10[i].on != last);
	}
	return changed;
}


void
drawx10(int i)
{
	if(x10[i].err || x10[i].locked){
		draw(screen, x10[i].r, red, nil, ZP);
		x10[i].err = 0;
	} else if(x10[i].on)
		draw(screen, x10[i].r, yellow, nil, ZP);
	else
		draw(screen, x10[i].r, lightblue, nil, ZP);

	_string(screen, addpt(x10[i].r.min, Pt(2,0)), display->black, ZP,
		font, x10[i].name, nil, strlen(x10[i].name), 
		x10[i].r, nil, ZP, SoverD);
	border(screen, x10[i].r, 1, display->black, ZP);	
}

void
x10togglelock(int i)
{
	Dir	d;

	nulldir(&d);
	if (x10[i].locked)
		d.mode = 0644;
	else
		d.mode = 0444;
	x10[i].locked = !x10[i].locked;
	dirwstat(x10[i].file, &d);
}

void
geometry(void)
{
	static int cols;
	int i, ncols, rows;
	Rectangle r;

	rows = (Dy(screen->r)-2*MARGIN+PAD)/(font->height*dsz+PAD);
	if(rows*cols < nx10 || rows*cols >= nx10*2){
		ncols = (nx10+rows-1)/rows;
		if(ncols != cols){
			cols = ncols;
		}
	}

	r = Rect(0,0,(Dx(screen->r)-2*MARGIN+PAD)/cols-PAD, font->height*dsz);
	for(i=0; i<nx10; i++)
		x10[i].r = rectaddpt(rectaddpt(r, Pt(MARGIN+(PAD+Dx(r))*(i/rows),
					MARGIN+(PAD+Dy(r))*(i%rows))), screen->r.min);

}
void
redraw(Image *screen)
{
	int i;

	draw(screen, screen->r, lightblue, nil, ZP);
	for(i=0; i<nx10; i++)
		drawx10(i);
	flushimage(display, 1);
}

void
eresized(int new)
{
	if(new && getwindow(display, Refmesg) < 0)
		fprint(2,"can't reattach to window");
	geometry();
	readx10();
	redraw(screen);
}

void
click(Mouse m)
{
	int i, j;
	int b2;

	if(m.buttons == 0 || (m.buttons & ~7))
		return;

	for(i=0; i<nx10; i++)
		if(ptinrect(m.xy, x10[i].r))
			break;
	if(i == nx10)
		return;

	b2 = (m.buttons&2);

	do
		m = emouse();
	while(m.buttons & 7);

	if(m.buttons != 0){
		do
			m = emouse();
		while(m.buttons);
		return;
	}

	for(j=0; j<nx10; j++)
		if(ptinrect(m.xy, x10[j].r))
			break;
	if(j != i)
		return;

	if (b2){
		x10togglelock(i);
	} else {
		x10[i].on = !x10[i].on;
		writex10(i);
		for(j=0; j<nx10; j++)
			x10[j].on = 0;
	}
	readx10();
	redraw(screen);
}

void
usage(void)
{
	fprint(2, "usage: x10/gui [[dir] file...]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	Event e;
	char *c;

	
	if(argc > 1){
		dir = *argv++;
		argc--;
	}
	if(argc > 1) {
		argv++; argc--;
		x10 = emalloc((argc)*sizeof(X10));
		while(argc--) {
			x10[argc].file = estrdup(argv[argc]);
			c = strrchr(x10[argc].file, '/');
			x10[argc].name = (c == nil ? x10[argc].file : c+1);
			x10[argc].on = -1;
			x10[argc].err= 0;
			nx10++;
		}
	} else 
		init();

	initdraw(0, 0, "x10/gui");
	lightblue = allocimagemix(display, DPalebluegreen, DWhite);
	if(lightblue == nil)
		sysfatal("allocimagemix: %r");
	yellow = allocimagemix(display, DYellow, DWhite);
	if(yellow == nil)
		sysfatal("allocimagemix: %r");
	red = allocimagemix(display, DRed, DWhite);
	if(red == nil)
		sysfatal("allocimagemix: %r");

	eresized(0);
	einit(Emouse|Ekeyboard|0x8);
	etimer(0x8, 10 * 1000);
	for(;;){
		switch(eread(Emouse|Ekeyboard|0x8, &e)){
		case Ekeyboard:
			if(e.kbdc==0x7F || e.kbdc=='q')
				exits(0);
			break;
		case Emouse:
			if(e.mouse.buttons)
				click(e.mouse);
			break;
		case 0x8:
			if (readx10())
				redraw(screen);
			break;
		}
	}
}

