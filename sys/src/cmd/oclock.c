#include <u.h>
#include <libc.h>
#include <thread.h>
#include <omero.h>
#include <error.h>
#include <draw.h>
#include <b.h>


char cmds[512];

Point
circlept(Point c, int r, int degrees)
{
	double rad;
	rad = (double) degrees * PI/180.0;
	c.x += cos(rad)*r;
	c.y -= sin(rad)*r;
	return c;
}

int
redraw(Panel* g)
{
	static char cmds[1024];
	char*	s;
	Point	c;
	int	rad;
	Point	cpt;
	int	i, r;
	int anghr, angmin;
	int tm;
	Tm tms;
	static int oanghr, oangmin;

	tm = time(0);
	tms = *localtime(tm);
	anghr = 90-(tms.hour*5 + tms.min/10)*6;
	angmin = 90-tms.min*6;
	if (oanghr == anghr && oangmin == angmin)
		return 0;
	oanghr = anghr;
	oangmin= angmin;
	c = Pt(40,40);
	rad= 35;
	s = cmds;
	s = seprint(s, cmds+1024, "ellipse %d %d %d %d grey\n", c.x, c.y, rad, rad);
	s = seprint(s, cmds+1024, "ellipse %d %d %d %d\n", c.x, c.y, rad, rad);
	for(i=0; i<12; i++){
		cpt = circlept(c, rad - 8, i*(360/12));
		s = seprint(s, cmds+1024,
			"ellipse %d %d %d %d yellow\n", cpt.x,  cpt.y, 2, 2);
	}
	cpt = circlept(c, (rad*3)/4, angmin);
	s = seprint(s, cmds+1024, "line %d %d %d %d 1 blue\n", c.x, c.y, cpt.x, cpt.y);
	cpt = circlept(c, rad/2, anghr);
	seprint(s, cmds+1024, "line %d %d %d %d 1 blue\n", c.x, c.y, cpt.x, cpt.y);
	openpanel(g, OWRITE|OTRUNC);
	r = writepanel(g, cmds, strlen(cmds));
	closepanel(g);
	return r;
}

void
omerogone(void)
{
	fprint(2, "%s: terminated\n", argv0);
	threadexitsall(nil);
}

void
threadmain(int argc, char* argv[])
{
	Panel*	d;

	ARGBEGIN{
	case 'd':
		omerodebug++;
		break;
	default:
		fprint(2, "usage: %s \n", argv0);
		sysfatal("usage");
	}ARGEND;
	if (argc > 0){
		fprint(2, "usage: %s\n", argv0);
		sysfatal("usage");
	}
	d = createpanel("oclock", "draw", nil);
	if (d == nil)
		sysfatal("createpanel: %r\n");
	panelctl(d, "tag");
	closepanelctl(d);
	for(;;){
		sleep(1000);
		if (redraw(d) < 0)
			threadexitsall(nil);
	}			
}
