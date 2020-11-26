#include <u.h>
#include <libc.h>
#include <thread.h>
#include <omero.h>
#include <plumb.h>
#include <error.h>
#include <b.h>


typedef struct Player	Player;
typedef struct Evh	Evh;

enum {
	KF=	0xF000,	/* Rune: beginning of private Unicode space */
	Spec=	0xF800,
	Kup=	KF|0x0E,
	Kdown=	Spec|0x00,
};

struct Player{
	char*	dir;
	char**	songs;
	int	nsongs;
	int	active;
	int	pause;
	int	pid;	// of player process

	Panel*	gtag;
	Panel*	gsongcol;
	Panel*	gsongs;
	Panel*	gpause;
};

struct Evh {
	char*	name;
	void	(*f)(Player*, Oev*);
};


Player p;

static char playbuf[4*1024];
static Channel*	plumbc;

void update(Player*);

void
omerogone(void)
{
	sysfatal("terminated");
}

static void
playproc(void *a)
{
	int	fd, afd;
	int	n, wn;
	int	playing;
	Player*	p = a;

	threadsetname("playproc");
again:
	if (p->active < 0 || p->active >= p->nsongs)
		threadexits("bug");
	fd = open(p->songs[p->active], OREAD);
	afd= open("/devs/audio/audio", OWRITE);
	if (fd < 0 || afd < 0){
		close(fd);
		close(afd);
		threadexits("open");
	}
	playing = p->active;
	do{
		if (p->pause){
			while(p->pause)
				sleep(100);
			if (playing != p->active){
				wn = -1;
				n = 0;
				break;
			}
		}
		wn = n = read(fd, playbuf, sizeof(playbuf));
		if (n > 0)
			wn = write(afd, playbuf, n);
	} while(n  > 0 && wn == n && playing == p->active);
	close(fd);
	close(afd);
	if (n < 0 || n != wn)
		threadexits("io");
	if (playing == p->active){
		p->active++;
		if (p->active == p->nsongs)
			p->active = 0;
		update(p);
		goto again;
	}
	threadexits(nil);
}

void
play(Player* p)
{
	proccreate(playproc, p, 16*1024);
}

void
stop(Player* p)
{
	p->active = -1;
}

static void
plumbimg(char* dir, char* arg)
{
	Plumbmsg*m;
	static int	plumbsendfd = -1;

	if (plumbsendfd < 0)
		plumbsendfd = open("/mnt/plumb/send", OWRITE|OCEXEC);
	if (plumbsendfd < 0)
		return;
	m = malloc(sizeof(Plumbmsg));
	if (m == nil)
		return;
	m->src = strdup(argv0);
	m->dst = strdup("poster");
	m->wdir= strdup(dir);
	m->type = strdup("text");
	m->attr = nil;
	m->data = smprint("%s/%s", dir, arg);
	m->ndata= -1;
	assert(m->wdir && m->src && m->data);
	plumbsend(plumbsendfd, m);
	plumbfree(m);
}

static int
mayplumbimg(char* dir, char* f)
{
	int	n;

	n = strlen(f);
	if (n < 5)
		return 0;
	n -= 4;
	if (!cistrcmp(f+n, ".jpg") || !strcmp(f+n, ".gif") || !strcmp(f+n, ".img") || !strcmp(f+n, ".png")){
		plumbimg(dir, f);
		return 1;
	}
	return 0;
}

int
readsongs(Player* p, char* dir)
{
	Dir*	ents;
	int	fd;
	int	i;
	char	buf[512];
	int	hasfiles;

	if (chdir(dir) < 0)
		return -1;
	getwd(buf, sizeof(buf));
	fd = open(".", OREAD);
	if (fd < 0)
		return -1;
	free(p->dir);
	p->dir = estrdup(buf);
	for (i = 0; i < p->nsongs; i++)
		free(p->songs[i]);
	free(p->songs);
	p->songs = 0;
	p->nsongs = dirreadall(fd, &ents);
	close(fd);
	if (p->nsongs < 0){
		return -1;
	}
	p->songs = emalloc(p->nsongs * sizeof(char*));
	hasfiles = 0;
	for (i = 0; i < p->nsongs; i++)
		if (ents[i].qid.type&QTDIR)
			p->songs[i] = smprint("%s/", ents[i].name);
		else {
			hasfiles++;
			p->songs[i] = estrdup(ents[i].name);
			mayplumbimg(p->dir, ents[i].name);
		}
	free(ents);
	p->active = -1;
	p->pause = 0;
	if (hasfiles)
		evhistory("oplayer", "look", p->dir);
	return hasfiles;
}

void
controls(Panel* w)
{
	Panel*	c;

	createsubpanel(w, "button:Play");
	createsubpanel(w, "button:Stop");
	createsubpanel(w, "button:Next");
	createsubpanel(w, "button:Done");
	p.gpause = createsubpanel(w, "button:Pause");
	c = createsubpanel(w, "slider:volume");
	openpanel(c, OWRITE);
	writepanel(c, "75", 2);
	closepanel(c);
	openpanelctl(c);
	panelctl(c, "tag");
	closepanelctl(c);
}


void
showlist(Player* p)
{
	int	i;
	char*	s;
	char*	txt;
	char*	mrk;
	char*	ap;

	txt = emalloc(32*1024);
	s = ap = txt;
	*txt = 0;
	for (i = 0; i < p->nsongs; i++){
		if (i != p->active)
			mrk = "";
		else {
			ap = s;
			mrk = "->";
		}
		s = seprint(s, txt+(32*1024), "%s\t%s\n", mrk, p->songs[i]);
	}
	if (!txt[0])
		strcpy(txt, "no songs.");
	openpanel(p->gsongs, OWRITE|OTRUNC);
	writepanel(p->gsongs, txt, strlen(txt));
	closepanel(p->gsongs);
	openpanelctl(p->gsongs);
	panelctl(p->gsongs, "sel %d %d\n", ap - txt, ap - txt);
	closepanelctl(p->gsongs);
	free(txt);
}

void
ui(Panel* w)
{
	Panel*	c;

	p.gtag = createsubpanel(w, "tag:player");
	c = createsubpanel(w, "row:controls");
	controls(c);
	p.gsongs = createsubpanel(w, "text:songs");
}

void
update(Player* p)
{
	char*	tag;

	tag = smprint("player — %s ..", p->dir ? p->dir : "no dir");
	openpanel(p->gtag, OWRITE|OTRUNC);
	writepanel(p->gtag, tag, strlen(tag));
	closepanel(p->gtag);
	free(tag);
	showlist(p);
}


int
isdir(char* p)
{
	Dir*	d;
	int	r;

	d = dirstat(p);
	if (d == nil)
		return 0;
	r = (d->qid.type&QTDIR);
	free(d);
	return r;
}

static int
findsong(Player* p, char* s)
{
	int	i;

	for (i = 0; i < p->nsongs; i++)
		if (!strcmp(p->songs[i], s))
			return i;
	// no exact match, try substring
	for (i = 0; i < p->nsongs; i++)
		if (strstr(p->songs[i], s))
			return i;

	return 0;
}

void
playfile(Player* p, char* file)
{
	char*	d;

	if (isdir(file)){
		if (readsongs(p, file) > 0){
			p->active = p->pause = 0;
			play(p);
		}
		update(p);
	} else if (access(file, AREAD) != -1){
		d = utfrrune(file, '/');
		if (d)
			*d++ = 0;
		readsongs(p, d ? file : ".");
		if (d){
			p->active = findsong(p, d);
			p->pause = 0;
			play(p);
		}
		update(p);
	}
}

void
elook(Player* p, Oev* e)
{
	char	str[40];
	int	vol;

	if (strstr(e->path, "/tag:")){
		if (readsongs(p, e->arg + 11 + 1) != -1)
			update(p);
		return;
	}
	if (strstr(e->path, "/text:songs")){
		if (isdir(e->arg + 11 + 1))
			readsongs(p, e->arg + 11 + 1);
		else {
			p->active = findsong(p, e->arg + 11 + 1);
			play(p);
		}
		update(p);
	}
	if (strstr(e->path, "/button:Play")){
		p->active = 0;
		play(p);
		update(p);
		return;
	}
	if (strstr(e->path, "/button:Stop")){
		stop(p);
		update(p);
		return;
	}
	if (strstr(e->path, "/button:Done")){
		stop(p);
		omeroterm();
		threadexitsall(nil);
	}
	if (strstr(e->path, "/button:Pause")){
		openpanel(p->gpause, OWRITE|OTRUNC);
		if (p->pause){
			writepanel(p->gpause,"Pause", 5);
			p->pause = 0;
		} else {
			writepanel(p->gpause,"Resume", 6);
			p->pause++;
		}
		closepanel(p->gpause);
		return;
	}
	if (strstr(e->path, "/button:Next")){
		p->active++;
		if (p->active == p->nsongs)
			p->active = 0;
		update(p);
		play(p);
		return;
	}
	if (strstr(e->path, "/slider:volume")){
		vol = atoi(e->arg);
		seprint(str, str+sizeof(str), "audio out %d", vol);
		writefstr("/devs/audio/volume", str);
	}
}

void
eexit(Player* , Oev* )
{
	omeroterm();
	threadexitsall(nil);
}

Evh evsh[] = {
	{ "Look",	elook },
	{ "look",	elook },
	{ "exit",	eexit },
	{ "exec",	elook },	// BUG
	{ "data",	elook },	// BUG
};


void
threadmain(int argc, char* argv[])
{
	Panel*	w;
	Channel* ec;
	Oev	e;
	Plumbmsg* m;
	int	i, ai;
	Alt	a[] = {
		{ nil, &e, CHANRCV },
		{ nil, &m, CHANRCV },
		{ nil, nil,CHANEND }};

	ARGBEGIN{
	case 'd':
		omerodebug++;
		break;
	default:
		fprint(2, "usage: %s [-d] [dir]\n", argv0);
		sysfatal("usage");
	}ARGEND;
	if (argc > 1){
		fprint(2, "usage: %s [-d] [dir]\n", argv0);
		sysfatal("usage");
	}
	ec = omeroeventchan(nil);
	a[0].c = ec;
	w = createpanel("oplayer", "col", nil);
	if (w == nil)
		sysfatal("createpanel: %r\n");
	plumbc = createportproc("song");
	a[1].c = plumbc;

	readsongs(&p, argc ? argv[0] : "/n/music");

	ui(w);

	update(&p);
	closepanelctl(w);
	while((ai = alt(a)) != -1){
		switch(ai){
		case 0:
			for (i = 0; i < nelem(evsh); i++)
				if (!strcmp(evsh[i].name, e.ev)){
					evsh[i].f(&p, &e);
					break;
				}
			clearoev(&e);
			break;
		case 1:
			playfile(&p, m->data);
			plumbfree(m);
			break;
		}
	}
	eexit(&p, nil);
}
