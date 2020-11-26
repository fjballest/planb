#include <u.h>
#include <libc.h>
#include <thread.h>
#include <omero.h>
#include <error.h>
#include <b.h>

typedef struct Vol Vol;

enum {
	Nvols = 128,
};

struct Vol {
	char*	mnt;
	char*	vols;
	int	changed;
	Panel*	g;
};

int	mainstacksize = 23 * 1024;
int	debug;
int	vflag;
#define	dprint	if(debug)fprint

Panel*	gcol;
Vol	vols[Nvols];
int	nvols;
char**	names;
int	nnames;

int
updatevol(char* ln)
{
	char*	mnt;
	char*	v;
	int	i;

	if (ln == nil || *ln == 0)
		return 0;
	mnt = ln;
	v = strchr(ln, '\t');
	if (v == nil)
		return 0;
	*v++ = 0;
	dprint(2, "updatevol: %s %s\n", mnt, v);
	if (nnames){
		for (i = 0; i < nnames; i++)
			if (!strcmp(names[i], mnt))
				break;
		if (i == nnames)
			return 0;
	}
	for (i = 0; i < nvols; i++)
		if (!strcmp(vols[i].mnt, mnt)){
			if (!strcmp(vols[i].vols, v))
				return 0;
			free(vols[i].vols);
			vols[i].vols = strdup(v);
			vols[i].changed = 1;
			return 1;
	}
	if (nvols == Nvols){
		fprint(2, "%s: too many vols\n", argv0);
	} else {
		vols[nvols].mnt = strdup(mnt);
		vols[nvols].vols = strdup(v);
		nvols++;
	}
	return 0;
}

int
update(void)
{
	static Qid oqid;
	int	p[2];
	int	tot, n;
	char	out[4096];
	char*	sc[Nvols*2];
	int	nsc;
	Dir*	d;
	int	changed, i;
	int	chg;

	changed = 0;
	d = dirstat("/dev/vol");
	if (d == nil)
		sysfatal("/dev/vol: %r\n");
	if (d->qid.vers == oqid.vers){
		free(d);
		return 0;
	}
	oqid = d->qid;

	if (pipe(p) < 0)
		sysfatal("pipe: %r");
	switch(rfork(RFPROC|RFFDG|RFNOWAIT)){
	case -1:
		sysfatal("rfork: %r");
		break;
	case 0:
		close(p[0]);
		dup(p[1], 1);
		close(p[1]);
		execl("/bin/vols", "vols", nil);
		sysfatal("exec vols: %r");
		break;
	default:
		close(p[1]);
		for(tot = 0; ; tot +=n){
			n = read(p[0], out+tot, sizeof(out) - tot);
			if (n <= 0)
				break;
		}
		close(p[0]);
		out[tot] = 0;
		nsc = gettokens(out, sc, nelem(sc), "\n");
		for (i = 0; i < nsc; i++){
			chg = updatevol(sc[i]);
			if (chg && vflag)
				fprint(2, "%s: changed: %s\n", argv0, sc[i]);
			changed |= chg;
		}
	}
	return changed;
}


void
updateui(int alert)
{
	int	i;
	char	nm[40];
	char	msg[40];

	for (i = 0; i < nvols; i++){
		if (vols[i].g == nil){
			seprint(nm, nm+40, "label:v%d", i);
			vols[i].g = createsubpanel(gcol, nm);
		}
		seprint(msg, msg+40, "%s: %s", vols[i].mnt, vols[i].vols);
		openpanel(vols[i].g, OWRITE|OTRUNC);
		writepanel(vols[i].g, msg, strlen(msg));
		closepanel(vols[i].g);
		openpanelctl(vols[i].g);
		panelctl(vols[i].g, vols[i].changed ? "font B" : "font R");
		closepanelctl(vols[i].g);
	}
	if (alert){
		for (i = 0; i < nvols; i++){
			if (vols[i].changed){
				openpanelctl(vols[i].g);
				panelctl(vols[i].g, "font B");
				closepanelctl(vols[i].g);
			}
		}
		openpanelctl(gcol);
		for (i = 0; i < 3; i++){
			panelctl(gcol, "hide");
			sleep(500);
			panelctl(gcol, "show");
			sleep(1000);
		}
		closepanelctl(gcol);
		sleep(10 * 1000);
		for (i = 0; i < nvols; i++){
			if (vols[i].changed){
				vols[i].changed = 0;
				openpanelctl(vols[i].g);
				panelctl(vols[i].g, "font R");
				closepanelctl(vols[i].g);
			}
		}
	}
}

void
usage(void)
{
	fprint(2, "usage: %s [-d] [-v] [file...]\n", argv0);
	exits("usage");
}

void
omerogone(void)
{
	sysfatal("terminated");
}

void
threadmain(int argc, char **argv)
{
	ARGBEGIN{
	case 'v':
		vflag = 1;
		break;
	case 'd':
		debug = 1;
		break;
	default:
		usage();
	}ARGEND;
	names = argv;
	nnames= argc;
	gcol = createpanel("ovols", "col", nil);
	if (gcol == nil)
		sysfatal("createpanel: %r\n");
	update();
	updateui(0);
	closepanelctl(gcol);
	for(;;){
		sleep(1000);
		if (update())
			updateui(1);
	}
}

