#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>
#include <plumb.h>
#include <ctype.h>
#include <omero.h>
#include "dat.h"

int	debug;
char	*maildir = "/mail/fs/";			/* mountpoint of mail file system */
char	*mailtermdir = "/mnt/term/mail/fs/";	/* alternate mountpoint */
char *mboxname = "mbox";			/* mailboxdir/mboxname is mail spool file */
char	*mailboxdir = nil;				/* nil == /mail/box/$user */
char *fsname;						/* filesystem for mailboxdir/mboxname is at maildir/fsname */
char	*user;
char	*outgoing;

Window	*wbox;
Message	mbox;
Message	replies;
char		*home;
int		plumbsendfd;
int		plumbseemailfd;
int		plumbshowmailfd;
int		plumbsendmailfd;
Channel	*cplumb;
Channel	*cplumbshow;
Channel	*cplumbsend;
int		wctlfd;
void		mainctl(void*);
void		plumbproc(void*);
void		plumbshowproc(void*);
void		plumbsendproc(void*);
void		plumbthread(void);
void		plumbshowthread(void*);
void		plumbsendthread(void*);

int			shortmenu;

void
omerogone(void)
{
	threadexitsall(nil);
}

void
usage(void)
{
	fprint(2, "usage:\tomail [-dsS] [-m maildir] [mailboxname]\n");
	fprint(2, "   or:\tomail -n [-o outgoing] [addr [subject]]\n");
	threadexitsall("usage");
}

void
removeupasfs(void)
{
	char buf[256];

	if(strcmp(mboxname, "mbox") == 0)
		return;
	snprint(buf, sizeof buf, "close %s", mboxname);
	write(mbox.ctlfd, buf, strlen(buf));
}

int
ismaildir(char *s)
{
	char buf[256];
	Dir *d;
	int ret;

	snprint(buf, sizeof buf, "%s%s", maildir, s);
	d = dirstat(buf);
	if(d == nil)
		return 0;
	ret = d->qid.type & QTDIR;
	free(d);
	return ret;
}

void
threadmain(int argc, char *argv[])
{
	char *s, *name;
	char err[ERRMAX];
	int i, newdir;
	int	sendonly;
	char*	subj;

	doquote = needsrcquote;
	quotefmtinstall();

	sendonly = 0;
	shortmenu = 0;
	ARGBEGIN{
	case 'n':
		sendonly = 1;
		break;
	case 'd':
		debug++;
		break;
	case 's':
		shortmenu = 1;
		break;
	case 'S':
		shortmenu = 2;
		break;
	case 'o':
		outgoing = EARGF(usage());
		break;
	case 'm':
		smprint(maildir, "%s/", EARGF(usage()));
		break;
	default:
		usage();
	}ARGEND
	if (sendonly){
		mbox.name = "/mail/";
		outgoing = smprint("/mail/box/%s/outgoing", getuser());
		if (argc > 1)
			subj = argv[1];
		else
			subj = nil;
		mkreply(nil, subj, "Mail", (argc ? argv[0] : ""), nil, nil);
		threadexits(nil);
	}
	/* open these early so we won't miss notification of new mail messages while we read mbox */
	plumbsendfd = plumbopen("send", OWRITE|OCEXEC);
	plumbseemailfd = plumbopen("seemail", OREAD|OCEXEC);
	plumbshowmailfd = plumbopen("showmail", OREAD|OCEXEC);

	name = "mbox";

	/* bind the terminal /mail/fs directory over the local one */
	if(access(maildir, 0)<0 && access(mailtermdir, 0)==0)
		bind(mailtermdir, maildir, MAFTER);

	newdir = 1;
	if(argc > 0){
		i = strlen(argv[0]);
		if(argc>2 || i==0)
			usage();
		/* see if the name is that of an existing /mail/fs directory */
		if(argc==1 && strchr(argv[0], '/')==0 && ismaildir(argv[0])){
			name = argv[0];
			mboxname = eappend(estrdup(maildir), "", name);
			newdir = 0;
		}else{
			if(argv[0][i-1] == '/')
				argv[0][i-1] = '\0';
			s = strrchr(argv[0], '/');
			if(s == nil)
				mboxname = estrdup(argv[0]);
			else{
				*s++ = '\0';
				if(*s == '\0')
					usage();
				mailboxdir = argv[0];
				mboxname = estrdup(s);
			}
			if(argc > 1)
				name = argv[1];
			else
				name = mboxname;
		}
	}

	user = getenv("user");
	if(user == nil)
		user = "none";
	if(mailboxdir == nil)
		mailboxdir = estrstrdup("/mail/box/", user);
	if(outgoing == nil)
		outgoing = estrstrdup(mailboxdir, "/outgoing");

	s = estrstrdup(maildir, "ctl");
	mbox.ctlfd = open(s, ORDWR|OCEXEC);
	if(mbox.ctlfd < 0)
		error("Mail: can't open %s: %r\n", s);

	fsname = estrdup(name);
	if(newdir && argc > 0){
		s = emalloc(5+strlen(mailboxdir)+strlen(mboxname)+strlen(name)+10+1);
		for(i=0; i<10; i++){
			sprint(s, "open %s/%s %s", mailboxdir, mboxname, fsname);
			if(write(mbox.ctlfd, s, strlen(s)) >= 0)
				break;
			err[0] = '\0';
			errstr(err, sizeof err);
			if(strstr(err, "mbox name in use") == nil)
				error("Mail: can't create directory %s for mail: %s\n", name, err);
			free(fsname);
			fsname = emalloc(strlen(name)+10);
			sprint(fsname, "%s-%d", name, i);
		}
		if(i == 10)
			error("Mail: can't open %s/%s: %r", mailboxdir, mboxname);
		free(s);
	}

	s = estrstrdup(fsname, "/");
	mbox.name = estrstrdup(maildir, s);
	mbox.level= 0;
	readmbox(&mbox, maildir, s);
	home = getenv("home");
	if(home == nil)
		home = "/";

	wbox = newwindow(mbox.name);
	wintagwrite(wbox, "Put Mail rm ", 3+1+4+1+7+1);
	threadcreate(mainctl, wbox, STACK);
	mbox.w = wbox;

	mesgmenu(wbox, &mbox);
	winclean(wbox);

	cplumb = chancreate(sizeof(Plumbmsg*), 0);
	cplumbshow = chancreate(sizeof(Plumbmsg*), 0);
	if(strcmp(name, "mbox") == 0){
		/*
		 * Avoid creating multiple windows to send mail by only accepting
		 * sendmail plumb messages if we're reading the main mailbox.
		 */
		plumbsendmailfd = plumbopen("sendmail", OREAD|OCEXEC);
		cplumbsend = chancreate(sizeof(Plumbmsg*), 0);
		proccreate(plumbsendproc, nil, STACK);
		threadcreate(plumbsendthread, nil, STACK);
	}
	/* start plumb reader as separate proc ... */
	proccreate(plumbproc, nil, STACK);
	proccreate(plumbshowproc, nil, STACK);
	threadcreate(plumbshowthread, nil, STACK);
	/* ... and use this thread to read the messages */
	plumbthread();
}

void
plumbproc(void*)
{
	Plumbmsg *m;

	threadsetname("plumbproc");
	for(;;){
		m = plumbrecv(plumbseemailfd);
		sendp(cplumb, m);
		if(m == nil)
			threadexits(nil);
	}
}

void
plumbshowproc(void*)
{
	Plumbmsg *m;

	threadsetname("plumbshowproc");
	for(;;){
		m = plumbrecv(plumbshowmailfd);
		sendp(cplumbshow, m);
		if(m == nil)
			threadexits(nil);
	}
}

void
plumbsendproc(void*)
{
	Plumbmsg *m;

	threadsetname("plumbsendproc");
	for(;;){
		m = plumbrecv(plumbsendmailfd);
		sendp(cplumbsend, m);
		if(m == nil)
			threadexits(nil);
	}
}

void
newmesg(char *name, char *digest)
{
	Dir *d;

	if(strncmp(name, mbox.name, strlen(mbox.name)) != 0)
		return;	/* message is about another mailbox */
	if(mesglookupfile(&mbox, name, digest) != nil)
		return;
	d = dirstat(name);
	if(d == nil)
		return;
	if(mesgadd(&mbox, mbox.name, d, digest))
		mesgmenu(wbox, &mbox);
	free(d);
}

void
showmesg(char *name, char *digest)
{
	char *n;

	if(strncmp(name, mbox.name, strlen(mbox.name)) != 0)
		return;	/* message is about another mailbox */
	n = estrdup(name+strlen(mbox.name));
	if(n[strlen(n)-1] != '/')
		n = egrow(n, "/", nil);
	mesgopen(&mbox, mbox.name, name+strlen(mbox.name), nil, digest);
	free(n);
}

void
delmesg(char *name, char *digest, int dodel)
{
	Message *m;

	m = mesglookupfile(&mbox, name, digest);
	if(m != nil){
		mesgmenumarkdel(wbox, &mbox, m, 0);
		if(dodel)
			m->writebackdel = 1;
	}
}

void
plumbthread(void)
{
	Plumbmsg *m;
	Plumbattr *a;
	char *type, *digest;

	threadsetname("plumbthread");
	while((m = recvp(cplumb)) != nil){
		a = m->attr;
		digest = plumblookup(a, "digest");
		type = plumblookup(a, "mailtype");
		if(type == nil)
			fprint(2, "Mail: plumb message with no mailtype attribute\n");
		else if(strcmp(type, "new") == 0)
			newmesg(m->data, digest);
		else if(strcmp(type, "delete") == 0)
			delmesg(m->data, digest, 0);
		else
			fprint(2, "Mail: unknown plumb attribute %s\n", type);
		plumbfree(m);
	}
	threadexits(nil);
}

void
plumbshowthread(void*)
{
	Plumbmsg *m;

	threadsetname("plumbshowthread");
	while((m = recvp(cplumbshow)) != nil){
		showmesg(m->data, plumblookup(m->attr, "digest"));
		plumbfree(m);
	}
	threadexits(nil);
}

void
plumbsendthread(void*)
{
	Plumbmsg *m;

	threadsetname("plumbsendthread");
	while((m = recvp(cplumbsend)) != nil){
		mkreply(nil,nil,  "Mail", m->data, m->attr, nil);
		plumbfree(m);
	}
	threadexits(nil);
}

int
mboxcommand(Window *w, char *s)
{
	char *args[10];
	Message *m, *next;
	int ok, nargs, i;
	char buf[128];

	s = strdup(s);
	nargs = tokenize(s, args, nelem(args));
	if(nargs == 0){
		free(s);
		return 0;
	}
	if(strcmp(args[0], "Mail") == 0){
		if(nargs == 1)
			mkreply(nil, nil, "Mail", "", nil, nil);
		else
			mkreply(nil, nil, "Mail", args[1], nil, nil);
		free(s);
		return 1;
	}
	if(strcmp(s, "Done") == 0){
		if(mbox.dirty){
			mbox.dirty = 0;
			fprint(2, "mail: mailbox not written\n");
			free(s);
			return 1;
		}
		ok = 1;
		for(m=mbox.head; m!=nil; m=next){
			next = m->next;
			if(m->w){
				if(windel(m->w))
					m->w = nil;
				else
					ok = 0;
			}
		}
		for(m=replies.head; m!=nil; m=next){
			next = m->next;
			if(m->w){
				if(windel(m->w))
					m->w = nil;
				else
					ok = 0;
			}
		}
		if(ok){
			windel(w);
			removeupasfs();
			threadexitsall(nil);
		}
		free(s);
		return 1;
	}
	if(strcmp(s, "Put") == 0){
		rewritembox(wbox, &mbox);
		mesgmenu(wbox, &mbox);
		free(s);
		return 1;
	}
	if(strcmp(s, "Rm") == 0){
		if(nargs > 1){
			for(i=1; i<nargs; i++){
				snprint(buf, sizeof buf, "%s%s", mbox.name, args[i]);
				delmesg(buf, nil, 1);
			}
		}
		free(s);
		return 1;
	}
	free(s);
	return 0;
}

void
mainctl(void *v)
{
	Window *w;
	Oev	e;
	Channel*	c;

	w = v;
	c = w->g->evc;
	while(recv(c, &e) != -1){
		dprint(2, "event %s %s\n", e.ev, e.arg);
		if (!strcmp(e.ev, "exec")){
			if(!mboxcommand(w, e.arg+12))	/* send it back */
				plumbexec(mbox.name, e.arg);
		} else if (!strcmp(e.ev, "look")){
			if (!mesgopen(&mbox, mbox.name, e.arg+12, nil, nil))
				plumblook(mbox.name, e.arg);
		} else if (!strcmp(e.ev, "exit")){
			clearoev(&e);
			break;
		}
		clearoev(&e);
	}
	chanfree(c);
	threadexits(nil);
}

