/*
 * Client for 9P file servers.
 *
 * Services calls that operate on a remote file tree.
 *
 * Requests are simply forwarded to the fs for processing.
 * The fs should NOT receive version/auth/attach requests. Those are
 * handled by the fs itself while being created.
 *
 * Attach, and recovery, are performed by the reader proc,
 * because this operation might block.
 *
 * This does NOT rewrite fids, and does NOT fix up qids. That's the
 * work of volfs. fs only services the 9P RPC
 *
 * From outside, a dead fs can be seen because it has a -1 fd and
 * always responds with Eio.
 *
 * BUG: Once created, a fs stays around. If there's an I/O error, the file
 * descriptor is closed, but the processes are kept running.
 */

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <auth.h>
#include <ip.h>
#include "names.h"
#include "vols.h"

enum {
	Request	= 0,
	Reply	= 1,
	Tick	= 2,
};

static QLock	fslck;
static Fs**	fss;
static int	nfss;
static uvlong	qidgen = 1;
static Lock	qidgenlck;
uvlong	Mvolqid	=	0x00000b4900000000ULL;	// B49

char	Eopen[] = "Fid already open";
char	Enotopen[] = "Fid not open";
char	Eio[] = "io error";
char	Eperm[] = "permission denied";
char	Enotexist[]="file does not exist";
char	Eauth[]	= "authentication not required";
char	Euser[]	= "permission denied for that user";
char	Espec[]	= "bad volume specifier";
char	Edupfid[]="fid already in use";
char	Ebadfid[]="bad fid";

static ulong	ndead, ncame, ntmout;
static ulong	nsrvrpcs[Tmax];
static ulong	nclirpcs[Tmax];
static char*	names[Tmax] = {
	[Tattach]	"attach",
	[Twalk]		"walk",
	[Topen]		"open",
	[Tclunk]	"clunk",
	[Tremove]	"remove",
	[Tread]		"read",
	[Twrite]	"write",
	[Tstat]		"stat",
	[Twstat]	"wstat",
	[Tflush]	"flush",
};

void
dumpfss(void)
{
	int	i;

	fprint(2, "fss:\n");
	qlock(&fslck);
	for (i = 0; i < nfss; i++){
		fprint(2, "   %p %s '%s' t=%ld ref=%ld fd=%d pid=%d e=%ld %ldms\n",
			fss[i], fss[i]->addr, fss[i]->spec,fss[i]->time, 
			fss[i]->ref, fss[i]->fd, fss[i]->pid, fss[i]->epoch,
			fss[i]->lat);
		if (fss[i]->fid)
			fprint(2, "\tQ=%llx %X\n", fss[i]->qid.path, fss[i]->fid);
	}
	qunlock(&fslck);
}

void
fsstats(void)
{
	int	i;
	ulong	tsrv, tcli;

	fprint(2, "fs stats:\n");
	fprint(2, "%ld deaths %ld recovers %ld tmouts\n", ndead, ncame, ntmout);
	tsrv = tcli = 0;
	for (i = 0; i < Tmax; i++){
		tsrv += nsrvrpcs[i];
		tcli += nsrvrpcs[i];
		if (names[i])
			fprint(2, "%s:\t%5ld cli\t%5ld srv\n",
				names[i], nclirpcs[i], nsrvrpcs[i]);
	}
	fprint(2,"total:\t%5ld cli\t%5ld srv\n", tcli, tsrv);
}

/* All calls to particular file systems must come through this.
 * This also dispatches to the empty volume and to the ctl fs.
 */
int
fsop(Fs* fs, Frpc* op)
{
	ulong r;

	if (fs == nil){
		// Fake, empty Fs used to present an empty dir
		// when a vol has a dead fs.
		return nilfsops[op->f.type](nil, op);
	}
	if (fs == &ctlfs){
		// fake fs to service the ctl file
		return ctlfsops[op->f.type](nil, op);
	}
	sendp(fs->reqc, op);
	r = recvul(op->sc);
	if (r == ~0)
		r = -1;
	return r;
}

void
fstimer(ulong now)
{
	int	i;
	Fs*	fs;

	qlock(&fslck);
	for (i = 0; i < nfss; i++){
		fs = fss[i];
		nbsendul(fs->tmrc, now);
	}
	qunlock(&fslck);
}

int
putfcall(int fd, Frpc* m)
{
	int	n;

	nsrvrpcs[m->f.type]++;
	Dbgprint(m->fid, "   <= %F\n", &m->f);
	n = convS2M(&m->f, m->sbuf, sizeof(m->sbuf));
	if (n > 0)
		n = write(fd, m->sbuf, n);
	else
		n = -1;
	return n;
}

int
putfrep(int fd, Frpc* m)
{
	int	n;

	if (m->r.type == Rstat && m->d){
		m->r.nstat = convD2M(m->d, m->buf, sizeof(m->buf));
		m->r.stat = m->buf;
		free(m->d);
		m->d = nil;
	}
	dbgprint(m->fid, "-> %F\n", &m->r);
	n = convS2M(&m->r, m->sbuf, sizeof(m->sbuf));
	if (n > 0)
		n = write(fd, m->sbuf, n);
	return n;
}

int
getfcall(int fd, Frpc* m)
{
	int	n;

	n = read9pmsg(fd, m->buf, sizeof(m->buf));
	if (n <= 0)
		return n;
	if (convM2S(m->buf, n, &m->f) == 0)
		return -1;
	nclirpcs[m->f.type]++;
	return n;
}

int
getfrep(int fd, Frpc* m)
{
	int	n;
	char*	p;

	n = read9pmsg(fd, m->buf, sizeof(m->buf));
	if (n <= 0)
		return n;
	if (convM2S(m->buf, n, &m->r) == 0)
		return -1;
	switch(m->r.type){
	case Rstat:
		free(m->d);
		m->d = emalloc(sizeof(Dir)+128);
		p = (char*)(m->d + 1);
		convM2D(m->r.stat, m->r.nstat, m->d, p);
		/* lib9p may insist that name is "/", which is not
		 * a valid file name
		 */
		if (m->d->name[0] == '/' && m->d->name[1] == 0)
			m->d->name = "dev";
		break;
	case Rerror:
		if (!strncmp(m->r.ename, "io ", 3))
			n = -1;
		break;
	}
	return n;
}

void
rpcerr(Frpc* fop, char* err)
{
	fop->r.type = Rerror;
	fop->r.tag = fop->f.tag;
	fop->r.ename = fop->err;
	strecpy(fop->err, fop->err+sizeof(fop->err), err);
}

static void
fsmuxrep(Fs* fs, Frpc* rep)
{
	Frpc*	fop;
	Frpc**	fl;
	int	e;

	for (fl = &fs->rpcs; fop = *fl; fl = &fop->next)
		if (fop->f.tag == rep->r.tag)
			break;
	if (fop == nil){
		rpcfree(rep);
		return;	// no tag. request was aborted.
	}
	*fl = fop->next;
	fop->next = nil;
	if (rep->r.type == Rread)
		memmove(fop->buf, rep->buf, rep->r.count);
	fop->r = rep->r;
	free(fop->d);
	fop->d = rep->d;
	rep->d = nil;
	if (fop->rep)
		rpcfree(fop->rep);
	fop->rep = rep;
	Dbgprint(fop->fid, "   => %F\n", &rep->r);
	e = 0;
	if (fop->r.type == Rerror){
		strecpy(fop->err, fop->err+sizeof(fop->err), fop->r.ename);
		fop->r.ename = fop->err;
		e = ~0;
	}
	sendul(fop->sc, e);	// awake the thread doing the rpc.
}

static int
maytimeout(Frpc* fop)
{
	/* This is here to experiment with removal of
	 * timeouts. Use with care. It may block the whole thing
	 * if a broken/faulty fs is not being timed out.
	 *
	 * As it is now, we timeout all requests but for those in
	 * volumes mounted with -T in spec that might block on
	 * devices like portfs: Tread.
	 */
	if (fop->flushed)
		return 1;

	if (fop->fid != nil && fop->fid->notimeout && fop->f.type == Tread)
		if (fop->fid->qid.path != 0LL)
			return (fop->fid->qid.type&QTDIR);
		else
			return 0;

	return 1;
}

static void
fstmouts(Fs* fs)
{
	Frpc*	fop;
	Frpc**	fl;
	int	some;

	some = 0;
	for (fl = &fs->rpcs; fop = *fl; ){
		if (maytimeout(fop) && fs->time - fop->time > Rpctmout){
			dprint(2, "timeout req tag %d type=%d ot=%uld ft=%uld\n",
				fop->f.tag, fop->f.type, fop->time, fs->time);
			ntmout++;
			*fl = fop->next;
			fop->next = nil;
			if (fop->flushed){
				rpcerr(fop, "interrupt");
			} else {
				if (fs->time - fop->time > 3 * Rpctmout){
					// don't abort the fs in this case.
					// It's likely that RPCs were very
					// old ones but the fs has recover.
					dprint(2, "%s: preposterous PRC tag %d type=%d\n", fs->addr, fop->f.tag, fop->f.type);
				} else
					some++;
				rpcerr(fop, Eio);
			}
			sendul(fop->sc, ~0);
		} else
			fl = &fop->next;
	}
	if (some){
		close(fs->fd);	// Shut this down to clean things up.
		fs->fd = -1;
		incref(&epoch);
	}
}

static void
abortrpcs(Fs* fs)
{
	Frpc*	fop;
	Frpc**	fl;

	for (fl = &fs->rpcs; fop = *fl; ){
		*fl = fop->next;
		fop->next = nil;
		rpcerr(fop, Eio);
		sendul(fop->sc, ~0);
	}
}


static void
fsreadproc(void*a)
{
	Fs*	fs = a;
	Frpc*	rep;
	int	r;
	vlong	t0, t1;
	long	tmout;

	threadsetname("fsread");

	assert(fs->fd < 0);
	fs->lat = Biglatency;
	t0 = nsec();
	if (!amountfs(fs))
		vdprint(2, "failed to mount %s %s\n", fs->addr, fs->spec);
	t1 = nsec();
	fs->lat = (t1 - t0)/1000000;
	tmout = 3000;
	for(;;){
		if (fs->fd < 0){
			vdprint(2, "trying %s %s\n", fs->addr, fs->spec);
			if (!amountfs(fs)){
				vdprint(2, "amount failed: %r\n");
				if (!fs->musthave && tmout < Trytmout * 1000)
					tmout += 1000;
				sleep(tmout);
				continue;
			}
			ncame++;
			vdprint(2, "got fs: %s %s\n", fs->addr, fs->spec);
			incref(&epoch);
			fs->epoch = epoch.ref;
		}
		tmout = 1000;
		rep = rpcalloc();
		r = getfrep(fs->fd, rep);
		if (r < 0){ 			// we're dying.
			vdprint(2, "readproc: dying\n");
			ndead++;
			close(fs->fd);
			fs->fd = -1;
			rpcfree(rep);
			rep = nil;		// Eio to all pending rpcs
			incref(&epoch);	
			fs->epoch = epoch.ref;
		}
		sendp(fs->repc, rep);
	}
	dprint(2, "fsreadproc exiting\n");
	threadexits(nil);
}


void
fsmuxthread(void*arg)
{
	Fs*	fs = arg;
	Frpc*	fop;
	Frpc*	rep;
	ulong	now;
	Alt	a[] = {	{ nil, &fop, CHANRCV},
			{ nil, &rep, CHANRCV},
			{ nil, &now, CHANRCV},
			{ nil, 0, CHANEND }};

	threadsetname("fsmux");
	a[0].c = fs->reqc;
	a[1].c = fs->repc;
	a[2].c = fs->tmrc;
	now = fs->time = time(nil);
	for(;;){
		rep = nil;
		switch(alt(a)){
		case Request:
			if (fs->fd < 0){
				rpcerr(fop, Eio);
				sendul(fop->sc, ~0);
			} else {
				fop->next = fs->rpcs;
				fs->rpcs = fop;
				fop->time = fs->time;
				putfcall(fs->fd, fop);
			}
			break;
		case Reply:
			if (rep != nil)
				fsmuxrep(fs, rep);
			else
				abortrpcs(fs);	// Eio to them all.
			break;
		case Tick:
			fs->time = now;
			fstmouts(fs);
			break;
		default:
			sysfatal("fsmux: alt failed\n");
		}
	}
	fprint(2, "fsmux exiting\n");
	threadexits(nil);
}

void
newfsqid(Qid* q)
{
	lock(&qidgenlck);
	qidgen++;
	q->path = (Mvolqid|(qidgen<<44));
	q->type = QTDIR;
	q->vers = 0;
	unlock(&qidgenlck);
}

static Fs*
addfs(char* addr, char* spec)
{
	int	i;
	Fs*	fs;

	vdprint(2, "adding fs for addr %s spec '%s'\n", addr, spec);
	if ((nfss%16) == 0)
		fss = erealloc(fss, sizeof(Fs*)*(nfss+16));
	i = nfss;
	fs = fss[i] = emalloc(sizeof(Fs));
	memset(fs, 0, sizeof(Fs));
	fs->addr = estrdup(addr);
	fs->spec = estrdup(spec);
	fs->reqc = chancreate(sizeof(Frpc*), 0);
	fs->repc = chancreate(sizeof(Frpc*), 0);
	fs->tmrc = chancreate(sizeof(ulong), 0);
	fs->fid = fidalloc(0);
	fs->fid->fs = fs;	// sic
	fs->fd = -1;
	fs->fdqid.path = -1;	// something weird for a qid.
	newfsqid(&fs->qid);
	/* A call to getfs made by the user may lead to this.
	 * The caller will already count its ref
	 */
	fs->ref = 0;
	fs->epoch = epoch.ref;
	fs->fid->epoch = fs->epoch;
	threadcreate(fsmuxthread, fs, Stack);
	fs->lat = Biglatency;
	fs->pid = proccreate(fsreadproc, fs, Stack);
	nfss++;

	/* We do not give fsreadproc much chance of mounting the fs.
	 * We don't do that ourselves and we dont synchronize
	 * because that might block all the program.
	 * For volume mounts that must be there for the system to
	 * work, mustmount should be specified.
	 * This is ugly, but waiting for all known volumes to be
	 * mounted may slow booting a lot. Also, after the mount
	 * request, the next 9p call is likely to find working vols
	 * already mounted.
	 */
	sleep(1);	// gives a chance.
	return fs;
}

Fs*
getfs(char* addr, char* spec)
{
	int	i;
	Fs*	c;

	qlock(&fslck);
	c = nil;
	for (i = 0; i < nfss; i++){
		if (!strcmp(addr, fss[i]->addr) && !strcmp(spec, fss[i]->spec)){
			c = fss[i];
			break;
		}
	}
	if (c == nil)
		c = addfs(addr, spec);
	if (c)
		incref(c);
	qunlock(&fslck);
	return c;
}

void
putfs(Fs* c)
{
	// The fs always stay there, once mounted.
	// It's likely that we will use them again and we keep them
	// already attached. To change this, unmount when no more refs.
	if (c)
		decref(c);
}
