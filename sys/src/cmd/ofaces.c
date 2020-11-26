#include <u.h>
#include <libc.h>
#include <thread.h>
#include <omero.h>
#include <error.h>
#include <plumb.h>
#include <regexp.h>
#include <ctype.h>
#include <b.h>

typedef struct Face Face;

struct Face {
	char*	key;
	char*	addr;
	char*	file;
	Panel*	col;
	Panel*	tag;
	Panel*	img;
	int	reclaim;	// for scandir
};

#define dprint if(debug)fprint

int	debug;
Channel*plumbc;
Panel*	grow;
Face**	faces;
int	nfaces;
int	maxfaces = 32;
char*	voiceinmsg;
char*	voiceoutmsg;

static char*
translatedomain(char *dom)
{
	static char buf[200];
	char *p, *ep, *q, *nextp, *file;
	char *bbuf, *ebuf;
	Reprog *exp;

	file = readfstr("/lib/face/.machinelist");
	if (file == nil)
		return dom;

	for(p=file; p; p=nextp) {
		if(nextp = strchr(p, '\n'))
			*nextp++ = '\0';

		if(*p == '#' || (q = strpbrk(p, " \t")) == nil || q-p > sizeof(buf)-2)
			continue;

		bbuf = buf+1;
		ebuf = buf+(1+(q-p));
		strncpy(bbuf, p, ebuf-bbuf);
		*ebuf = 0;
		if(*bbuf != '^')
			*--bbuf = '^';
		if(ebuf[-1] != '$') {
			*ebuf++ = '$';
			*ebuf = 0;
		}

		if((exp = regcomp(bbuf)) == nil){
			fprint(2, "bad regexp in machinelist: %s\n", bbuf);
			sysfatal("regexp");
		}

		if(regexec(exp, dom, 0, 0)){
			free(exp);
			ep = p+strlen(p);
			q += strspn(q, " \t");
			if(ep-q+2 > sizeof buf) {
				fprint(2, "huge replacement in machinelist: %.*s\n", utfnlen(q, ep-q), q);
				sysfatal("bad big replacement");
			}
			strncpy(buf, q, ep-q);
			ebuf = buf+(ep-q);
			*ebuf = 0;
			while(ebuf > buf && (ebuf[-1] == ' ' || ebuf[-1] == '\t'))
				*--ebuf = 0;
			free(file);
			return buf;
		}
		free(exp);
	}
	free(file);

	return dom;

}

static char*
tryfindpicture_user(char *dom, char *user, int depth)
{
	static char buf[200];
	char *p, *q, *nextp, *file, *usr;
	usr = getuser();

	sprint(buf, "/usr/%s/lib/face/48x48x%d/.dict", usr, depth);
	if((file = readfstr(buf)) == nil)
		return nil;

	snprint(buf, sizeof buf, "%s/%s", dom, user);

	for(p=file; p; p=nextp) {
		if(nextp = strchr(p, '\n'))
			*nextp++ = '\0';

		if(*p == '#' || (q = strpbrk(p, " \t")) == nil)
			continue;
		*q++ = 0;

		if(strcmp(buf, p) == 0) {
			q += strspn(q, " \t");
			q = buf+snprint(buf, sizeof buf, "/usr/%s/lib/face/48x48x%d/%s", usr, depth, q);
			while(q > buf && (q[-1] == ' ' || q[-1] == '\t'))
				*--q = 0;
			free(file);
			return buf;
		}
	}
	free(file);
	return nil;			
}

static char*
tryfindpicture_global(char *dom, char *user, int depth)
{
	static char buf[200];
	char *p, *q, *nextp, *file;

	sprint(buf, "/lib/face/48x48x%d/.dict", depth);
	if((file = readfstr(buf)) == nil)
		return nil;

	snprint(buf, sizeof buf, "%s/%s", dom, user);

	for(p=file; p; p=nextp) {
		if(nextp = strchr(p, '\n'))
			*nextp++ = '\0';

		if(*p == '#' || (q = strpbrk(p, " \t")) == nil)
			continue;
		*q++ = 0;

		if(strcmp(buf, p) == 0) {
			q += strspn(q, " \t");
			q = buf+snprint(buf, sizeof buf, "/lib/face/48x48x%d/%s", depth, q);
			while(q > buf && (q[-1] == ' ' || q[-1] == '\t'))
				*--q = 0;
			free(file);
			return buf;
		}
	}
	free(file);
	return nil;			
}

static char*
tryfindpicture(char *dom, char *user, int depth)
{
	char* result;

	if((result = tryfindpicture_user(dom, user, depth)) != nil)
		return result;

	return tryfindpicture_global(dom, user, depth);
}

static char*
tryfindfile(char *dom, char *user, int depth)
{
	char *p, *q;

	for(;;){
		for(p=dom; p; (p=strchr(p, '.')) && p++)
			if(q = tryfindpicture(p, user, depth))
				return q;
		depth >>= 1;
		if(depth == 0)
			break;
	}
	return nil;
}

void
parseaddr(char* sender, char** dp, char** up)
{
	char*	dom;
	char*	user;
	char *at, *bang;
	char *p;


	/* works with UTF-8, although it's written as ASCII */
	for(p=sender; *p!='\0'; p++)
		*p = tolower(*p);
	user = sender;
	dom = nil;
	at = strchr(sender, '@');
	if(at){
		*at++ = '\0';
		dom = at; // estrdup(at);
		goto done;
	}
	bang = strchr(sender, '!');
	if(bang){
		*bang++ = '\0';
		user = bang; // estrdup(bang);
		dom = sender;
	}
done:
	*dp = dom;
	*up = user;
}

char*
dblookup(char* a)
{
	static char	*facedom;
	char*	dom;
	char*	user;
	char*	p;
	static char addr[80];

	strecpy(addr, addr+80, a);
	parseaddr(addr, &dom, &user);

	if(facedom == nil){
		facedom = getenv("facedom");
		if(facedom == nil)
			facedom = "astro";
	}
	if(dom == nil)
		dom = facedom;
	dom = translatedomain(dom);
	dprint(2,"dom %s\n", dom);
	if(p = tryfindfile(dom, user, 8))
		return p;
	p = tryfindfile(dom, "unknown", 8);
	if(p != nil || strcmp(dom, facedom)==0)
		return p;
	p =  tryfindfile("unknown", "unknown", 8);
	if (!p)
		return "/dev/null";
	else
		return p;
}

void
omerogone(void)
{
	fprint(2, "%s: terminated\n", argv0);
	threadexitsall(nil);
}

void
freeface(Face* f)
{
	free(f->key);
	free(f->addr);
	free(f->file);
	openpanelctl(grow);
	panelctl(grow, "hold");
	if (f->img)
		removepanel(f->img);
	if (f->tag)
		removepanel(f->tag);
	if (f->col)
		removepanel(f->col);
	closepanelctl(grow);
	free(f);
}

Face*
newface(char* addr, char* key)
{
	int	i;
	int	pos = -1;
	Face*	f;

	for (i = 0; i < nfaces; i++){
		if (faces[i] == nil)
			pos = i;
		else if (key && !strcmp(faces[i]->key, key))
			return nil;	// already there
	}
	if (pos < 0){
		if ((nfaces%32) == 0)
			faces = erealloc(faces, (nfaces+32)*sizeof(Face*));
		pos = nfaces++;
	}
	f = faces[pos] = emalloc(sizeof(Face));
	f->key = key ? estrdup(key) : nil;
	f->addr = estrdup(addr);
	f->file = estrdup(dblookup(addr));
	f->reclaim = 0; // for scandir
	dprint(2,"new face addr %s key %s file %s\n", f->addr, f->key, f->file);
	if (f->file == nil){
		faces[pos] = nil;
		freeface(f);
		f = nil;
	}
	return f;
}

void
say(char* fmt, char* name)
{
	char	voice[50];
	char*	s;

	seprint(voice, voice+sizeof(voice), fmt, name);
	s = strchr(voice, '@');
	if (s)
		*s = 0;
	writefstr("/devs/voice/output", voice);

}

void
delfacei(int i)
{
	Face*	f;

	f = faces[i];
	dprint(2, "ofaces: del %s\n", f->addr);
	if (voiceoutmsg)
		say(voiceoutmsg, f->addr);
	faces[i] = nil;
	freeface(f);
}

void
delface(char* key)
{
	int	i;

	for (i = 0; i < nfaces; i++)
		if (faces[i] && !strcmp(faces[i]->key, key)){
			delfacei(i);
			break;
		}
}

int
hasface(char* key)
{
	int	i;

	for (i = 0; i < nfaces; i++)
		if (faces[i] && !strcmp(faces[i]->key, key)){
			faces[i]->reclaim = 0;	// for scandir
			return 1;
		}
	return 0;
}

void
addfaceimg(Face* f)
{
	long	l;
	void*	data;
	char	addr[8];
	char*	s;

	if (f == nil)
		return;
	openpanelctl(grow);
	panelctl(grow, "hold");
	f->col = createsubpanel(grow, "col:face");
	if (f->col == nil){
		fprint(2, "ofaces: %r\n");
		goto fail;
	}
	openpanelctl(f->col);
	panelctl(f->col, "notag");
	closepanelctl(f->col);
	f->img = createsubpanel(f->col, "image:face");
	if (f->img == nil){
		fprint(2, "ofaces: %r\n");
		goto fail;
	}
	closepanelctl(f->img);
	f->tag = createsubpanel(f->col, "label:sender");
	if (f->tag == nil){
		fprint(2, "ofaces: %r\n");
		goto fail;
	}
	openpanelctl(f->tag);
	panelctl(f->tag, "font S");
	closepanelctl(f->tag);
	seprint(addr, addr+sizeof(addr), f->addr);
	s = strchr(addr, '@');
	if (s)
		*s = 0;
	if (openpanel(f->tag, OWRITE) < 0)
		dprint(2, "openpanel %s %r\n", f->tag->name);
	if (writepanel(f->tag, addr, strlen(addr)) != strlen(addr))
		dprint(2, "writepanel %s %r\n", f->tag->name);
	closepanel(f->tag);
	data = readf(f->file, nil, 0, &l);
	if (data && l>0){
		openpanel(f->img, OWRITE);
		writepanel(f->img, data, l);
		closepanel(f->img);
	}
	free(data);
	dprint(2, "ofaces: add %s\n", f->addr);
	if (voiceinmsg)
		say(voiceinmsg, f->addr);
fail:
	closepanelctl(grow);
}

/* cmd output must be a list of mail addresses for users.
 * All white space is ignored.
 * Optionally, each address may be prefixed by a string and
 * an equal sign, to permit multiple addresses to be shown
 * multiple times.
 * Example:
 *  	nemo paurea esoriano
 * Another example:
 * 	/mail/box/nemo/mails/432/231/text=lscore@lsub.org
 */
void
getusers(char* cmd)
{
	static	char	buf[8*1024];
	static	char*	names[512];
	static	char*	keys[512];
	char*	s;
	int	nnames, i, n;
	char*	c;

	if (cmd[0] != '/' && cmd[0] != '.')
		c = smprint("/bin/%s", cmd);
	else
		c = strdup(cmd);
	n = tcmdoutput(c, buf, sizeof(buf)-1);
	free(c);
	if (n <= 0)
		return;
	buf[n] = 0;
	nnames = tokenize(buf, keys, nelem(keys));
	for (i = 0; i < nnames; i++){
		s = utfrune(keys[i], '=');
		if (s == nil){
			names[i] = keys[i];
		} else {
			*s++ = 0;
			names[i] = s;
		}
	}
	for (i = 0; i < nfaces; i++)
		if (faces[i])
			faces[i]->reclaim = 1;
	dprint(2, "ofaces: ");
	for (i = 0; i < nnames; i++){
		dprint(2, "%s ", names[i]);
		hasface(keys[i]);
	}
	dprint(2, "\n");
	for (i = 0; i < nfaces; i++)
		if (faces[i] && faces[i]->reclaim)
			delfacei(i);
	for (i = 0; i < nnames && nfaces < maxfaces; i++)
		if (!hasface(keys[i]))
			addfaceimg(newface(names[i], keys[i]));
}

void
lookmail(Oev* e)
{
	static int showfd = -1;
	int	i;
	char*	s;

	for (i = 0; i < nfaces; i++)
		if (faces[i] && faces[i]->img == e->panel)
			break;
	if (i == nfaces){
		dprint(2, "no mail for look event?");
		return;
	}
	dprint(2, "look for mail from %s\n", faces[i]->key);
	if (showfd == -1)
		showfd = plumbopen("edit", OWRITE);
	s = nil;
	if (faces[i]->addr){
		s = smprint("ofaces\nedit\n/\ntext\naddr=\n%ld\n%s",
			strlen(faces[i]->key), faces[i]->key);
		if (showfd != -1){
			if (write(showfd, s, strlen(s)) < 0){
				close(showfd);
				showfd = -1;
			}
		}
	}
	free(s);
}

void
usage(void)
{
	fprint(2, "usage: %s [-d] [-n max] [-i inmsg] [-o outmsg] [-l label] cmd\n", argv0);
	sysfatal("usage");
}

void
timerproc(void* a)
{
	Channel* c = a;

	for(;;){
		sleep(15 * 1000);
		sendul(c, 0);
	}
}

void
threadmain(int argc, char* argv[])
{
	extern int omerodebug;
	char* label;
	char*	cmd;
	char*	s;
	char	str[40];
	Panel*	lg;
	Oev	e;
	long	l;
	int	lflag;
	Alt	a[] = {
		{ nil, &e, CHANRCV },
		{ nil, &l, CHANRCV },
		{ nil, nil, CHANEND }};

	lflag = 0;
	label = nil;
	ARGBEGIN{
	case 'i':
		voiceinmsg = EARGF(usage());
		break;
	case 'o':
		voiceoutmsg = EARGF(usage());
		break;
	case 'd':
		debug++;
		if (debug > 1)
			omerodebug++;
		break;
	case 'n':
		maxfaces = atoi(EARGF(usage()));
		break;
	case 'l':
		lflag++;
		label = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;
	switch(argc){
	case 0:
		cmd = strdup("Mails -l");
		break;
	case 1:
		cmd = strdup(argv[0]);
		break;
	default:
		cmd = nil;
		usage();
	}
	if (label == nil){
		label = strdup(cmd);
		s = strchr(label, ' ');
		if (s)
			*s = 0;
		s = strrchr(label ,'/');
		if (s)
			label = s+1;
	}
	a[0].c = omeroeventchan(nil);
	a[1].c = chancreate(sizeof(ulong), 0);
	grow = createpanel((lflag ? label : "ofaces"), "row", nil);
	if (grow == nil)
		sysfatal("panelinit: %r\n");
	if (label){
		seprint(str, str+40, "label:%s", label);
		lg = createsubpanel(grow, str);
		if (lg == nil)
			sysfatal("createsubpanel: %r");
	}
	panelctl(grow, "nohold");	// BUG: back compat. remove.
	closepanelctl(grow);
	proccreate(timerproc, a[1].c, 8*1024);
	getusers(cmd);
	for(;;){
		switch(alt(a)){
		case 0:
			dprint(2, "event %s %s\n", e.ev, e.path);
			if (!cistrcmp(e.ev, "look"))
				lookmail(&e);
			clearoev(&e);
			break;
		case 1:
			getusers(cmd);
			break;
		default:
			goto done;
		}
	}
done:
	omeroterm();
	threadexitsall(nil);
}
