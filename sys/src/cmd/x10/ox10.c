#include <u.h>
#include <libc.h>
#include <thread.h>
#include <omero.h>
#include <error.h>
#include <b.h>

typedef struct X10 X10;
struct X10 {
	char *name;
	char *file;
	Panel* g;
	int on;
	int err;
	int locked;
};

X10 x10[50];
int nx10;

char *dir = "/devs/x10";
Panel*gtop;

void
addx10(char* f)
{
	int i, j, fd, nr;
	Dir*	d;
	Dir*	pd;
	char*	fn;
	char	xname[50];

	if((fd = open(f, OREAD)) < 0)
		return;
	d = dirfstat(fd);
	if (d == nil){
		close(fd);
		return;
	}
	if (d->qid.type&QTDIR){
		nr = dirreadall(fd, &pd);
		for(i=0; i<nr; i++)
			if (strcmp(pd[i].name,"cm11"))
			if (*pd[i].name == 'w' || *pd[i].name == 'p'){
				fn = smprint("%s/%s", f, pd[i].name);
				addx10(fn);
				free(fn);
			}
		free(pd);
		free(d);
		close(fd);
		return;
	}
	j = nx10++;
	assert(nx10 < nelem(x10));
	x10[j].file = estrdup(f);
	x10[j].name = strrchr(f, '/');
	if (x10[j].name)
		x10[j].name = strdup(x10[j].name + 1);
	else
		x10[j].name = strdup(f);
	x10[j].on = 0;
	seprint(xname, xname+sizeof(xname), "button:%s", x10[j].name);
	x10[j].g = createsubpanel(gtop, xname);
	if (x10[j].g == nil)
		sysfatal("omero");
	openpanel(x10[j].g, OWRITE|OTRUNC);
	writepanel(x10[j].g, x10[j].name, strlen(x10[j].name));
	closepanel(x10[j].g);
	free(d);
	close(fd);
}

void
writex10(int i)
{
	char*	str;

	str = ( x10[i].on ? "on" : "off");
	if (writefstr(x10[i].file, str) < 0)
		x10[i].err = 1;
}

int
updatex10(void)
{
	int i;
	char buf[80];
	long l;
	int changed;
	int last;

	changed = 0;
	for (i = 0; i < nx10; i++){
		last = x10[i].on;
		l = sizeof(buf)-1;
		readf(x10[i].file, buf, l, &l);
		x10[i].on = 0;
		if (l < 0)
			x10[i].err = 1;
		else if (strcmp(buf, "on") == 0)
			x10[i].on = 1;
		changed |= (x10[i].err || x10[i].on != last);
	}
	return changed;
}

void
drawx10(void)
{
	int	i;

	openpanelctl(gtop);
	panelctl(gtop, "hold");
	for (i = 0; i < nx10; i++){
		openpanelctl(x10[i].g);
		if (x10[i].on)
			panelctl(x10[i].g, "font B");
		else
			panelctl(x10[i].g, "font R");
		closepanelctl(x10[i].g);
	}
	closepanelctl(gtop);
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
toggle(Oev* g)
{
	int	i;

	for (i = 0; i < nx10; i++)
		if (x10[i].g == g->panel && !x10[i].locked){
			x10[i].on = !x10[i].on;
			writex10(i);
			drawx10();
			break;
		}
}

void
protect(Oev* g)
{
	int	i;
	char	nam[40];

	for (i = 0; i < nx10; i++)
		if (x10[i].g == g->panel){
			x10togglelock(i);
			openpanel(x10[i].g, OWRITE|OTRUNC);
			if(x10[i].locked){
				seprint(nam, nam+sizeof(nam), "[%s]", x10[i].name);
				writepanel(x10[i].g, nam, strlen(nam));
			} else
				writepanel(x10[i].g, x10[i].name, strlen(x10[i].name));
			closepanel(x10[i].g);
			drawx10();
			break;
		}
}

void
timerproc(void*a)
{
	Channel*tc = a;

	for(;;){
		sleep(3*1000);
		sendul(tc, 0);
	}
}

void
usage(void)
{
	fprint(2, "usage: x10/gui  [file...]\n");
	exits("usage");
}

int
omerogone(void)
{
	sysfatal("gone");
	return 0;
}

void
threadmain(int argc, char *argv[])
{
	Oev e;
	int	t;
	Alt	a[] = {
		{nil, &e, CHANRCV},
		{nil, &t, CHANRCV},
		{nil, nil, CHANEND},
	};

	ARGBEGIN{
	default:
		usage();
	}ARGEND;

	gtop = createpanel("x10", "col", nil);
	if (gtop == nil)
		sysfatal("initgraph");

	if (argc == 0)
		addx10("/devs/x10");
	else
		while(argc--)
			addx10(*argv++);
	closepanelctl(gtop);

	a[0].c = omeroeventchan(nil);
	a[1].c = chancreate(sizeof(ulong), 0);
	proccreate(timerproc, a[1].c, 8*1024);
	updatex10();
	drawx10();
	for(;;){
		switch(alt(a)){
		case 0:
			if (!strcmp(e.ev, "look"))
				protect(&e);
			else if (!strcmp(e.ev, "exec"))
				toggle(&e);
			clearoev(&e);
			break;
		case 1:
			if (updatex10())
				drawx10();
			break;
		default:
			sysfatal("alt");
		}
	}
}

