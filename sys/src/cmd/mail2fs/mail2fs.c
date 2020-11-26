#include "common.h"
#include <libsec.h>
#include <ctype.h>
#include <thread.h>
#include <b.h>
#include "all.h"

typedef struct Exec Exec;

enum
{
	STACK		= 8192,
};

enum
{
	NARGS		= 100,
	NARGCHAR	= 8*1024,
	EXECSTACK 	= STACK+(NARGS+1)*sizeof(char*)+NARGCHAR
};
struct Exec
{
	char		*prog;
	char		**argv;
	int		p[2];	/* p[1] is write to program; p[0] set to prog fd 0*/
	int		q[2];	/* q[0] is read from program; q[1] set to prog fd 1 */
	Channel	*sync;
};

int debug = 0;
int paranoid = 0;

char *goodtypes[] = {
	"text",
	"text/plain",
	"message/rfc822",
	"text/richtext",
	"text/tab-separated-values",
	"application/octet-stream",
	nil,
};

int
fieldcmp(String* s, char* c)
{
	return cistrcmp(s_to_c(s), c);
}

void
mesgline(int fd, char *header, String *value)
{
	char*	s;

	if (value == nil || fd < 0)
		return;
	if(s_len(value) > 0){
		s = malloc(s_len(value)+2);
		decquoted(s, s_to_c(value), s_to_c(value)+s_len(value));
		fprint(fd, "%s: %s", header, s);
		free(s);
	}
}

void
checkpath(char* fn)
{
	static char* pref;

	if (pref == nil)
		pref = smprint("/usr/%s/mail/", getuser());
	if (paranoid && strncmp(fn, pref, strlen(pref)))
		sysfatal("path %s not in $home/mail/", fn);
}

void
writeattach(char* fn, char* data, long len)
{
	int	fd;
	
	checkpath(fn);
	if (access(fn, AEXIST) >= 0){
		fprint(2, "fn: %s: file exists\n", fn);
	} else {
		fd = create(fn, OWRITE, 0660);
		if (fd < 0){
			fprint(2, "fn: %s: %r\n", fn);
			return;
		}
		if (write(fd, data, len) != len)
			fprint(2, "fn: %s: %r\n", fn);
		close(fd);
	}
}

int
mimedisplay(Message *m, char *name, int fileonly)
{
	char *dest;
	char*	fn;
	char*	p;
	if (!m)
		return 0;
	if(m->disposition == Dfile  || (m->filename && s_len(m->filename)!=0)){
		if(s_len(m->filename) == 0){
			if (strlen(m->name) == 0)
				dest = strdup("attach");
			else
				dest = strdup(m->name);
			p = strchr(dest, '\n');
			if (p) *p = 0;
		}else
			dest = strdup(s_to_c(m->filename));
		
		if (access(name, AEXIST) < 0)
			fn = smprint("%s.%s", name, dest);
		else
			fn = smprint("%s/%s", name, dest);
		writeattach(fn, m->body, m->bend - m->body);
		free(fn);
		free(dest);
		return 1;
	}else if(!fileonly)
		print("\tfile is %sbody\n", name); // , ext(m->type)
	return 0;
}

/* copy argv to stack and free the incoming strings, so we don't leak argument vectors */
void
buildargv(char **inargv, char *argv[NARGS+1], char args[NARGCHAR])
{
	int i, n;
	char *s, *a;

	s = args;
	for(i=0; i<NARGS; i++){
		a = inargv[i];
		if(a == nil)
			break;
		n = strlen(a)+1;
		if((s-args)+n >= NARGCHAR)	/* too many characters */
			break;
		argv[i] = s;
		memmove(s, a, n);
		s += n;
		free(a);
	}
	argv[i] = nil;
}

void
execproc(void *v)
{
	struct Exec *e;
	int p[2], q[2];
	char *prog;
	char *argv[NARGS+1], args[NARGCHAR];

	e = v;
	p[0] = e->p[0];
	p[1] = e->p[1];
	q[0] = e->q[0];
	q[1] = e->q[1];
	prog = e->prog;	/* known not to be malloc'ed */
	rfork(RFFDG);
	sendul(e->sync, 1);
	buildargv(e->argv, argv, args);
	free(e->argv);
	chanfree(e->sync);
	free(e);
	dup(p[0], 0);
	close(p[0]);
	close(p[1]);
	if(q[0]){
		dup(q[1], 1);
		close(q[0]);
		close(q[1]);
	}
	procexec(nil, prog, argv);
//fprint(2, "exec: %s", e->prog);
//{int i;
//for(i=0; argv[i]; i++) print(" '%s'", argv[i]);
//print("\n");
//}
//argv[0] = "cat";
//argv[1] = nil;
//procexec(nil, "/bin/cat", argv);
	fprint(2, "Mail: can't exec %s: %r\n", prog);
	threadexits("can't exec");
}

char*
formathtml(char *body, int *np)
{
	int i, j, p[2], q[2];
	Exec *e;
	char buf[1024];
	Channel *sync;

	e = emalloc(sizeof(struct Exec));
	if(pipe(p) < 0 || pipe(q) < 0)
		return("pipe");

	e->p[0] = p[0];
	e->p[1] = p[1];
	e->q[0] = q[0];
	e->q[1] = q[1];
	e->argv = emalloc(4*sizeof(char*));
	e->argv[0] = strdup("htmlfmt");
	e->argv[1] = strdup("-cutf-8");
	e->argv[2] = strdup("-a");
	e->argv[3] = nil;
	e->prog = "/bin/htmlfmt";
	sync = chancreate(sizeof(int), 0);
	e->sync = sync;
	proccreate(execproc, e, EXECSTACK);
	recvul(sync);
	close(p[0]);
	close(q[1]);

	if((i=write(p[1], body, *np)) != *np){
		fprint(2, "Mail: warning: htmlfmt failed: wrote %d of %d: %r\n", i, *np);
		close(p[1]);
		close(q[0]);
		return body;
	}
	close(p[1]);

	body = nil;
	i = 0;
	for(;;){
		j = read(q[0], buf, sizeof buf);
		if(j <= 0)
			break;
		body = realloc(body, i+j+1);
		if(body == nil)
			return nil;
		memmove(body+i, buf, j);
		i += j;
		body[i] = '\0';
	}
	close(q[0]);

	*np = i;
	return body;
}


int
isprintable(String *type)
{
	int i;

	if (!type)
		return 0;
	for(i=0; goodtypes[i]!=nil; i++)
		if(strcmp(s_to_c(type), goodtypes[i])==0)
			return 1;
	return 0;
}

void decode(Message*);

int
datafile(char* dir)
{
	int	fd;
	char*	fn;

	checkpath(dir);

	if (access(dir, AREAD) < 0){
		fd = create(dir, OREAD, DMDIR|0750);
		if (fd < 0)
			return -1;
		close(fd);
	}
	fn = smprint("%s/text", dir);
	fd = open(fn, OWRITE);
	if (fd < 0)
		fd = create(fn, OWRITE, 0660);
	if (fd >= 0)
		seek(fd, 0, 2);
	free(fn);
	return fd;
}

int
mkmesgfs(Message* m, char* dir)
{
	char *s, *subdir, *name;
	Message *mp, *thisone;
	int n, body;
	int dfd;
	if (m == nil)
		return 1;

	//print("*** at %s (%s)\n", dir, m->type ? s_to_c(m->type) : "none");
	dfd = -1;
	/* suppress headers of envelopes */
	if(fieldcmp(m->type, "message/rfc822") != 0){
		dfd = datafile(dir);
		if (dfd < 0)
			return 0;
		mesgline(dfd, "From", m->from822);
		mesgline(dfd, "Date", m->date822);
		mesgline(dfd, "To", m->to822);
		mesgline(dfd, "CC", m->cc822);
		mesgline(dfd, "Bcc", m->bcc822);
		mesgline(dfd, "Reply-To", m->replyto822);
		mesgline(dfd, "Subject", m->subject822);
		fprint(dfd, "\n");
	}

	if(m->part == nil){	/* single part message */
		if(fieldcmp(m->type, "text")==0||
		    (m->type && strncmp(s_to_c(m->type), "text/", 5)==0)){
			mimedisplay(m, dir, 1);
			if (dfd < 0)
				dfd = datafile(dir);
			n = m->bend - m->body;
			if (fieldcmp(m->type, "text/html") == 0){
				s = formathtml(m->body, &n);
				if (n > 0)
					write(dfd, s, n);
				free(s);
			} else 
				if (n > 0)
					write(dfd, m->body, n);
		}else
			mimedisplay(m, dir, 0);
	}else{
		/* multi-part message, either multipart/* or message/rfc822 */
		thisone = nil;
		if(fieldcmp(m->type, "multipart/alternative") == 0){
			thisone = m->part;	/* in case we can't find a good one */
			for(mp=m->part; mp!=nil; mp=mp->next)
				if(isprintable(mp->type)){
					thisone = mp;
					break;
				}
			body=0;
		} else
			body = 1;
		for(mp=m->part; mp!=nil; mp=mp->next){
			if(thisone!=nil && mp!=thisone)
				continue;
			/* For multipart/arternative kludges,
			 * consider as text all the inner ones as well.
			 */
			if (fieldcmp(mp->type, "multipart/alternative")==0)
				subdir = strdup(dir);
			else
				if (thisone || body){
					subdir = strdup(dir);
					body = 0;
				} else
					subdir = smprint("%s/%s", dir, mp->name);
			name = strdup(mp->name);
			/* skip first element in name because it's already in window name */
			dprint(2, "\n===> %s \n",name);
			if(fieldcmp(mp->type, "text")==0 ||
			   (mp->type && strncmp(s_to_c(mp->type), "text/", 5)==0)){
				if (!mimedisplay(mp, dir, 1)){
					if (dfd < 0)
						dfd = datafile(dir);
					if (dfd < 0)
						return 0;
					mesgline(dfd, "From", mp->from822);
					mesgline(dfd, "Date", mp->date822);
					mesgline(dfd, "To", mp->to822);
					mesgline(dfd, "CC", mp->cc822);
					mesgline(dfd, "Bcc", mp->bcc822);
					mesgline(dfd, "Subject", mp->subject822);
					mesgline(dfd, "Reply-To", mp->replyto822);
					fprint(dfd, "\n");
					n = mp->bend - mp->body;
					if (fieldcmp(mp->type, "text/html") == 0){
						s = formathtml(mp->body, &n);
						if (n > 0)
							write(dfd, s, n);
						free(s);
					} else
						if (n > 0)
							write(dfd, mp->body, n);
				}
			}else{
				if((mp->type &&
					strncmp(s_to_c(mp->type), "multipart/", 10)==0 ) ||
				   fieldcmp(mp->type, "message/rfc822")==0){
					//print("*** recur\n");
					if (!mkmesgfs(mp, subdir))
						return 0;
				}else
					mimedisplay(mp, subdir, 0);
			}
			free(name);
			free(subdir);
		}
	}
	if (dfd != -1)
		close(dfd);
	return 1;
}

static char*
datedir(void)
{
	static char dname[4+2+1];
	Tm*	tm;

	tm = localtime(time(nil));
	if (tm == nil)
		strcpy(dname, "000000");
	else
		seprint(dname, dname+sizeof(dname), "%04d%02d",
			tm->year + 1900, tm->mon+1);
	return dname;
}

int
mkmboxfs(Mailbox* mb, char* dir)
{
	Message*m;
	char*	d;
	int	nbfd;
	char*	nbfn;
	char	buf[40];
	int	nb;
	char	e[50];
	int	n;
	char*	r;
	char*	digests;
	char*	mdir;

	close(create(dir, OREAD, DMDIR|0700));

	nbfn = smprint("%s/seq", dir);
	mdir = smprint("%s/%s", dir, datedir());
	close(create(mdir, OREAD, DMDIR|0700));
	while((nbfd = open(nbfn, ORDWR)) < 0){
		if (nbfd >=0)
			break;
		rerrstr(e, sizeof(e));
		if (strstr(e, "exclusive"))
			sleep(2000);
		else
			break;
	}
	if (nbfd < 0)
		nbfd = create(nbfn, ORDWR, DMEXCL|0664);
	free(nbfn);
	if (nbfd < 0){
		fprint(2, "can't open seq file");
		free(mdir);
		return 0;
	}
	n = read(nbfd, buf, sizeof(buf)-1);
	if (n < 0)
		n = 0;
	buf[n] = 0;
	nb = (int)strtod(buf, &r);
	nbfn = smprint("%s/digests", dir);
	digests = readfstr(nbfn);
	if (!digests)
		digests = strdup("");
	for(m = mb->root->part; m; m = m->next){
		if (strstr(digests, s_to_c(m->sdigest)))
			continue;
		else
			digests = smprint("%s%s\n", digests, s_to_c(m->sdigest));
		d=smprint("%s/%d", mdir, nb++);
		if (!mkmesgfs(m, d)){
			free(d);
			free(nbfn);
			free(digests);
			free(mdir);
			return 0;
		}
		free(d);
	}
	if (writefstr(nbfn, digests) < 0)
		createf(nbfn, digests, strlen(digests), 0664);
	seek(nbfd, 0, 0);
	fprint(nbfd, "%08d", nb);
	close(nbfd);
	free(nbfn);
	free(digests);
	free(mdir);
	return 1;
}

char*
cleanpath(char* file, char* dir)
{
	char*	s;
	char*	t;

	assert(file && file[0]);
	if (file[1])
		file = strdup(file);
	else {
		s = file;
		file = malloc(3);
		file[0] = s[0];
		file[1] = 0;
	}
	s = cleanname(file);
	if (s[0] != '/' && dir != nil && dir[0] != 0){
		t = smprint("%s/%s", dir, s);
		free(s);
		s = cleanname(t);
	}
	return s;
}

static void
usage(void)
{
	fprint(2, "usage: %s [-D] [-n] [-d dir] [mbox]\n", argv0);
	threadexits("usage");
}

int mainstacksize = 32 * 1024;

void
threadmain(int argc, char*argv[])
{
	Mailbox*	mb;
	Mlock*	lk;
	char*	s;
	char*	mbox;
	Dir	d;
	char*	tmp;
	char*	dir;
	int	fd;
	int	mode;
	int	dry;
	char	wdir[512];

	dry = 0;
	dir = nil;
	ARGBEGIN{
	case 'n':
		dry++;
		break;
	case 'd':
		dir = EARGF(usage());
		break;
	case 'D':
		debug++;
		break;
	default:
		usage();
	}ARGEND;
	if (argc == 1){
		getwd(wdir, sizeof(wdir));
		mbox = cleanpath(argv[0], wdir);
		if (access(mbox, AREAD) < 0){
			free(mbox);
			mbox = cleanpath(argv[0], smprint("/mail/box/%s", getuser()));
		}
	} else {
		mbox = smprint("/mail/box/%s/mbox", getuser());
		if (argc != 0)
			usage();
	}
	if (dir == nil)
		dir = smprint("/mail/box/%s/mails", getuser());

	/* Avoid interrupts in the middle of the process
	 * when called from programs that scan for new
	 * mail every once in a while
	 */
	rfork(RFNOTEG);

	mb = newmbox(mbox, strrchr(mbox, '/') + 1, 1);
	if (mb == nil)
		sysfatal("no mbox");
	if (dry){
		// Just try to generate the fs; don't update anything
		readmbox(mb);
		mkmboxfs(mb, dir);
		exits(nil);
	}
	lk = syslock(mbox);
	mode = DMAPPEND|DMEXCL|0622;
	tmp = smprint("%s.tmp", mbox);
	sysremove(tmp);
	fd = create(tmp, ORDWR, mode);
	nulldir(&d);
	d.mode = mode;
	dirwstat(tmp, &d);
	if (fd < 0){
		fprint(2, "%s: error in temp file: %r\n", argv0);
		goto fail;
	}
	close(fd);
	s = readmbox(mb);
	if (s){
		fprint(2, "readmbox: %s", s);
		goto fail;
	}
	if (!mkmboxfs(mb, dir))
		goto fail;
	sysremove(mbox);
	if (sysrename(tmp, mbox) < 0){
		fprint(2, "%s: can't rename temp file: %r\n", argv0);
		goto fail;
	}
	if (lk)
		sysunlock(lk);
	exits(nil);
fail:
	if (lk)
		sysunlock(lk);
	exits("fail");
}
