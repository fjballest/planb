#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <auth.h>
#include <9p.h>
#include <error.h>
#include <b.h>

/* Caching CS for Plan B.
 * This is a caching version of ndb/cs that talks to
 * the real /net/cs file using slave processes.
 * If a request is cached, cs is not even touched.
 * When ndb/cs blocks due to a / held by bns(8),
 * ccs still resolves all the names seen. This allows
 * factotum and bns to resolve names without requiring a
 * working /. 
 */

typedef struct Ent Ent;
typedef struct Cli Cli;

struct Ent {
	Ent*	next;
	char*	req;
	char*	rep;
};

struct Cli {
	Req*	r;
	Channel*reqc;
	int	csfd;
	char*	req;
	char*	rep;
	Cli*	next;
};

char*	cssrvname = "/srv/cs_net";
QLock	lck;
Cli*	clients;
RWLock	elck;
Ent*	ents;
int	debug;

static char*
lookup(char* req, int n)
{
	Ent*	e;
	char*	r;

	r = nil;
	rlock(&elck);
	if (debug)
		print("ccs: lookup %.*s\n", n, req);
	for (e = ents; e ; e = e->next){
		if (!strncmp(e->req, req, n) && e->req[n] == 0){
			r = estrdup(e->rep);
			if (debug)
				print("\t=> %s\n", r);
			break;
		}
	}
	runlock(&elck);
	return r;
}

static void
install(char* req, char* rep, int nrep)
{
	Ent*	e;

	e = emalloc(sizeof(Ent));
	e->req = estrdup(req);
	e->rep = emalloc(nrep+1);
	strncpy(e->rep, rep, nrep);
	e->rep[nrep] = 0;
	wlock(&elck);
	e->next = ents;
	ents = e;
	wunlock(&elck);
}

static void
flush(void)
{
	Ent*	e;

	wlock(&elck);
	while(ents != nil){
		e = ents;
		ents = e->next;
		free(e->req);
		free(e->rep);
		free(e);
	}
	wunlock(&elck);
}

static void
fsflush(Req* r)
{
	Cli*	c;
	Req*	rr;

	qlock(&lck);
	for (c = clients; c != nil; c = c->next)
		if (c->r && c->r->tag == r->ifcall.oldtag){
			rr = c->r;
			c->r = nil;
			qunlock(&lck);
			respond(rr, "flush");
			respond(r, nil);
			return;
		}
	qunlock(&lck);
	fprint(2, "%s: no old req found\n", argv0);
}

static void
fsclunk(Fid* fid)
{
	Cli*	c;
	Cli**	cl;

	/* When we get a clunk, there's no
	 * outstanding read or write, and there cannot be
	 * an on-going request. BUG: flush.
	 */
	c = fid->aux;
	if (c != nil){
		qlock(&lck);
		for (cl = &clients; *cl; cl = &(*cl)->next)
			if (*cl == c){
				*cl = c->next;
				break;
			}
		if (c->reqc)
			sendp(c->reqc, nil);
		else {
			if (c->csfd >= 0)
				close(c->csfd);
			free(c->req);
			free(c->rep);
			free(c);
		}
		qunlock(&lck);
	}
}

static void
fsopen(Req* r)
{
	Fid*	fid;
	Cli*	c;

	fid = r->fid;
	if (fid->qid.type&QTDIR){
		respond(r, nil);
		return;
	}
	c = emalloc(sizeof(Cli));
	memset(c, 0, sizeof(Cli));
	c->csfd = -1;
	r->fid->aux = c;
	qlock(&lck);
	c->next = clients;
	clients = c;
	qunlock(&lck);
	respond(r, nil);
}

/* Read all answers found by the real ndb/cs,
 * and place them one-per-line.
 */
static long
csread(int fd, char* buf, long n)
{
	long	r;
	long	nr;

	n-= 2; // for \n and for \0
	r = 0;
	while(r < n){
		nr = read(fd, buf+r, n-r);
		if (nr < 0)
			return -1;
		if (nr == 0)
			break;
		r += nr;
		buf[r++] = '\n';
	}
	if (r > 0 && buf[r-1] == '\n')
		buf[r-1] = 0;
	else
		buf[r] = 0;
	return r;
}

/* Service a read using a previously read buffer.
 * This is like readstr, but returns just one cs reply
 * at a time, to mimic what ndb/cs does.
 */
static void
csreadstr(Req* r, char* buf)
{
	long	n;
	char*	e;
	vlong	off;

	/* Don't count our fake \n in the offset */
	for(off = r->ifcall.offset; *buf != 0 && off > 0; buf++)
		if (*buf != '\n')
			off--;
	while(*buf == '\n')
		buf++;
	if (*buf == 0){
		r->ofcall.count = 0;
		return;
	}
	e = strchr(buf, '\n');
	if (e)
		n = e - buf;
	else
		n = strlen(buf);
	if (r->ifcall.count <= n)
		r->ofcall.count = r->ifcall.count;
	else
		r->ofcall.count = n;
	memmove(r->ofcall.data, buf, r->ofcall.count);
	
}

static void
reqproc(void* a)
{
	Req*	r;
	Cli*	c;
	Fid*	fid;
	long	n;
	Channel*reqc = a;
	char*	buf;

	threadsetname("reqproc");
	c = nil;
	while(r = recvp(reqc)){
		fid = r->fid;
		c = fid->aux;
		c->r = r;
		switch(r->ifcall.type){
		case Twrite:
			if (c->csfd < 0)
				c->csfd = open("/n/cs/cs", ORDWR);
			if (c->csfd < 0){
				if (c->r == r)
					responderror(r);
				goto Done;
			}
			n = r->ifcall.count;
			n = pwrite(c->csfd, r->ifcall.data, n, 0);
			r->ofcall.count = n;
			if (c->r == r)
				if (n < 0){
					install(c->req, "no match", 8);
					responderror(r);
				} else
					respond(r, nil);
			break;
		case Tread:
			if (c->csfd < 0){
				if (c->r == r)
					respond(r, "you should write first");
				goto Done;
			}
			if (!r->ifcall.offset) {
				buf = emalloc(16*1024);
				n = csread(c->csfd, buf, 16*1024);
				if (n <= 0){
					install(c->req, "no match", 8);
					free(buf);
					if (c->r == r)
						responderror(r);
					goto Done;
				} else if (n > 0){
					install(c->req, buf, n);
				}
				free(buf);	
			}
			buf = lookup(c->req, strlen(c->req));
			if (buf){
				csreadstr(r, buf);
				respond(r, nil);
			} else {
				r->ofcall.count = 0;
				respond(r, nil);
			}
			break;
		default:
			sysfatal("bad request %d", r->ifcall.type);
		}
	Done:
		c->r = nil;
	}
	if(chatty9p)
		fprint(2, "client exiting\n");
	if (c){
		if (c->csfd >= 0)
			close(c->csfd);
		free(c->req);
		free(c->rep);
		chanfree(c->reqc);
		free(c);
	}
	threadexits(nil);
}

static void
fsread(Req* r)
{
	Fid*	fid;
	Cli*	c;

	fid = r->fid;
	c = fid->aux;
	if (c->rep != nil){
		if (!strcmp(c->rep,"no match"))
			respond(r, "no match");
		else {
			csreadstr(r, c->rep);
			respond(r, nil);
		}
	} else if (c->reqc == nil)
		respond(r, "you should read first");
	else 
		sendp(c->reqc, r);
}

static void
fswrite(Req* r)
{
	Fid*	fid;
	Cli*	c;
	char*	s;

	fid = r->fid;
	c = fid->aux;
	if (r->ifcall.offset != 0LL){
		respond(r, "bad request: offset not zero");
		return;
	}
	r->ofcall.count = r->ifcall.count;
	if (!strncmp(r->ifcall.data, "flush" , 5)){
		flush();
		respond(r, nil);
	} else if (!strncmp(r->ifcall.data, "debug" , 5)){
		debug = 1;
		respond(r, nil);
	} else if (!strncmp(r->ifcall.data, "nodebug" , 7)){
		debug = 0;
		respond(r, nil);
	} else if (s = lookup(r->ifcall.data, r->ifcall.count)){
		free(c->rep);
		c->rep = s;
		if (!strcmp(c->rep, "no match"))
			respond(r, "no match");
		else
			respond(r, nil);
	} else {
		if (c->reqc == nil){
			c->reqc = chancreate(sizeof(Req*), 0);
			procrfork(reqproc, c->reqc, 8*1024, RFMEM);
		}
		free(c->req);
		c->req = emalloc(r->ifcall.count+1);
		c->req[r->ifcall.count] = 0;
		strncpy(c->req, r->ifcall.data, r->ifcall.count);
		sendp(c->reqc, r);
	}
}

static void
fsattach(Req* r)
{
	int	fd;

	fd = open(cssrvname, ORDWR);
	if (fd < 0){
		responderror(r);
		return;
	}
	if (mount(fd, -1, "/n/cs", MREPL, "") < 0){
		responderror(r);
		return;
	}
	close(fd);
	respond(r, nil);
}

static Srv sfs=
{
	.attach	=	fsattach,
	.open	=	fsopen,
	.read	=	fsread,
	.write	=	fswrite,
	.flush	= 	fsflush,
	.destroyfid = 	fsclunk,
};
void
usage(void)
{
	fprint(2,"usage: %s [-d] [-s srv] [-m mnt] cssrv\n",argv0);
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	char*	mnt;
	char*	srv;
	int	mflag;
	File*	f;

	srv = "cs";
	mnt = "/net";
	mflag = MAFTER;
	ARGBEGIN{
	case 'd':
		if (++debug > 1)
			chatty9p++;
		break;
	case 's':
		srv = EARGF(usage());
		break;
	case 'm':
		mnt = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;
	if (argc == 1){
		cssrvname = argv[0];
		argc--;
	}
	if (argc != 0)
		usage();


	if (!chatty9p)
		rfork(RFNOTEG);
	sfs.tree =  alloctree(nil, nil, DMDIR|0755, nil);
	f = createfile(sfs.tree->root, "cs", getuser(), 0666, nil);
	closefile(f);
	threadpostmountsrv(&sfs, srv, mnt, mflag);

	/* Alert the user about the caching version of ndb/cs */
	print("ndb/ccs... \n");

	threadexits(nil);
}
