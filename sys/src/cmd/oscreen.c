#include <u.h>
#include <libc.h>
#include <thread.h>
#include <omero.h>
#include <error.h>
#include <b.h>

typedef struct Scr Scr;


enum {
	Nscr = 10,
};

struct Scr {
	char*	name;
	char*	addr;
	Panel*	g;
};

Panel*	gcol;
Scr	scr[Nscr];
int	nscr;
char*	current;
int	refresh;

Scr	adds[Nscr];
int	nadds;

void
init(int nsc, char* sc[])
{
	int	i, j;
	char*	e;
	char*	p;

	for(i = 0; i < nscr; i++){
		if (scr[i].name == nil)
			break;
		free(scr[i].name);
		free(scr[i].addr);
		scr[i].name = nil;
		scr[i].addr = nil;
	}
	if (nsc > Nscr){
		nsc = Nscr;
		fprint(2, "screen: Nscr overflow\n");
	}
	for(i=0; i<nsc; i++){
		if (e = strchr(sc[i], '='))
			*e++ = 0;
		scr[i].name = estrdup(sc[i]);
		if (e){
			scr[i].addr = estrdup(e);
			p = strstr(scr[i].addr, "mouse");
			if (p)
				strcpy(p, "11000");
		}
	}
	nscr = nsc;
	for (i = 0; i < nadds; i++){
		for(j = 0; j < nscr; j++)
			if (scr[j].name && !strcmp(scr[j].name, adds[i].name)){
				free(scr[j].addr);
				scr[j].addr = estrdup(adds[i].addr);
				break;
			}
		if (j == nscr){
			scr[nscr].name = estrdup(adds[i].name);
			scr[nscr].addr = estrdup(adds[i].addr);
			nscr++;
		}
	}
}

void
update(void)
{
	char	out[200];
	char*	sc[Nscr];
	int	nsc, n;

	n = tcmdoutput("/bin/env/terms -m", out, sizeof(out)-1);
	if (n < 0)
		return;
	out[n] = 0;
	nsc = tokenize(out, sc, nelem(sc));
	init(nsc, sc);
}

int
setscr(char *name)
{
	int fd;

	if((fd = open("/dev/mousectl", OWRITE)) < 0){
		fprint(2, "cannot open /dev/mousectl: %r");
		return -1;
	}
	fprint(fd, "call %s\n", name);
	close(fd);
	return 0;
}

int
setkbd(char *name)
{
	int fd;

	if((fd = open("/dev/kbdctl", OWRITE)) < 0){
		fprint(2, "cannot open /dev/kbdctl: %r");
		return -1;
	}
	fprint(fd, "call %s\n", name);
	close(fd);
	return 0;
}

void
updateui(void)
{
	int	i;
	char	nm[40];

	for (i = 0; i < Nscr; i++){
		if (scr[i].name){
			if (!scr[i].g){
				seprint(nm, nm+40, "label:m%d", i);
				scr[i].g = createsubpanel(gcol, nm);
			}
			if (scr[i].g == nil)
				continue;
			openpanel(scr[i].g, OWRITE|OTRUNC);
			writepanel(scr[i].g, scr[i].name, strlen(scr[i].name));
			closepanel(scr[i].g);
		} else if (scr[i].g){
			removepanel(scr[i].g);
			scr[i].g = nil;
		}
	}
}

void
click(int i)
{
	char*	ld;
	char	old;

	free(current);
	current = estrdup(scr[i].name);
	if (scr[i].addr){
		setscr(scr[i].addr);
		ld = scr[i].addr + strlen(scr[i].addr) - 1;
		old = *ld;
		*ld = old + 1;
		setkbd(scr[i].addr);
		*ld = old;
	} else
		setscr(scr[i].name);
	openpanelctl(gcol);
	panelctl(gcol, "hold");
	if (refresh)
		update();
	updateui();
	closepanelctl(gcol);
}

void
usage(void)
{
	fprint(2, "usage: %s [-a machine=addr] [machine...]\n", argv0);
	exits("usage");
}

int
omerogone(void)
{
	sysfatal("terminated");
	return 0;
}

void
threadmain(int argc, char **argv)
{
	Oev	e;
	char	dir[80];
	Channel*	c;
	int	i;
	char*	p;

	ARGBEGIN{
	case 'a':
		strecpy(dir, dir+sizeof(dir), EARGF(usage()));
		p = strchr(dir, '=');
		if (!p)
			usage();
		*p++ = 0;
		adds[nadds].name = strdup(dir);
		adds[nadds++].addr = strdup(p);
		break;
	default:
		usage();
	}ARGEND;
	if (argc != 0)
		init(argc, argv);
	else {
		update();
		refresh = 1;
	}
	current = estrdup(sysname());
	c = omeroeventchan(nil);
	gcol = createpanel("oscreen", "row",  nil);
	if (gcol == nil)
		sysfatal("createpanel: %r\n");
	updateui();
	closepanelctl(gcol);
	while(recv(c, &e) != -1){
		if (!strcmp(e.ev, "exit")){
			threadexitsall(nil);
		} else if (!strcmp(e.ev, "look") || !strcmp(e.ev, "exec")){
			for (i = 0; i < nscr; i++)
				if (e.panel == scr[i].g){
					click(i);
					break;
				}
		}
		clearoev(&e);
	}
	threadexitsall(nil);
}

