/*
 * Implements the RPCs for a Plan B volume
 * relying on fs.c, ctlfs.c, or nilfs.c.
 *
 * Locking order:
 *	1st. fids are locked by the RPCs using them.
 *	2nd. volslck. 
 * 	3rd. mvol locks or vol mntlck
 * In general:
 *	do not lock fids while holding a vol/mvol lock.
 *	do not lock volslck while holding a mvol/vol lock.
 */

#include <u.h>
#include <libc.h>
#include <auth.h> 
#include <ip.h>
#include <fcall.h>
#include <thread.h>
#include "names.h"
#include "vols.h"


/* We use our own fids for servers, because we may
 * have to (re)attach to them and we don't know which fids
 * the client will use in the future.
 * All fids owned by us have 0 as their client nr.
 * The alloc routine returns a new fid each time we alloc 0.
 *
 * Regarding Rpcs used for our own bookkeeping, we try to
 * use the client supplied rpc. See the comment near the top
 * of main.c
 */

static QLock	fidlck;
static int	fidgen;
static Fid*	fidhash[Fhashsz];
static Fid*	freefids;
static ulong	nfids;
static ulong	nafids;

int	msglen;

#define FIDHASH(s)	fidhash[(s)%Fhashsz]

int	hdebug;

void
dbgprint(Fid* fid, char* msg, ...)
{
	va_list	arg;

	if (!debug && (!fid || !fid->debug))
		return;
	va_start(arg, msg);
	vfprint(2, msg, arg);
	va_end(arg);
}

void
Dbgprint(Fid* fid, char* msg, ...)
{
	va_list	arg;

	if (debug < 2 && (!fid || fid->debug < 2))
		return;
	va_start(arg, msg);
	vfprint(2, msg, arg);
	va_end(arg);
}

int
fidfmt(Fmt* fmt)
{
	Fid*	f;

	f = va_arg(fmt->args, Fid*);
	if (!f)
		return fmtprint(fmt, "nil fid");
	return fmtprint(fmt, "0x%p: fid=%d sfid=%d %N"
			" mv=%p fs=%p e=%d Q=%llx %c %c",
			f, f->nr, f->snr, f->sname,
			f->mvol, f->fs, f->epoch, f->qid.path,
			((f->qid.type&QTDIR) ? 'd' : ' '),
			(f->notimeout ? 'n' : 't'));
}

static Fid *
getfid(int nr)
{
	Fid *f;

	qlock(&fidlck);
	for(f = FIDHASH(nr); f; f = f->hnext)
		if(f->nr == nr)
			break;
	qunlock(&fidlck);
	return f;
}

int
getfidnr(void)
{
	int	nr;

	qlock(&fidlck);
	nr = ++fidgen;
	qunlock(&fidlck);
	return nr;
}

void
closefid(Fid* f, Frpc* fop)
{
	/* We keep f->isopen as it was.
	 * This is only to close the fid in the server.
	 */
	if (f->fs && f->fs != &ctlfs && f->fs->fd >= 0){
		fop->f.type = Tclunk;
		fop->f.fid = f->snr;
		fsop(f->fs, fop);
	}
	putfs(f->fs);
	f->fs = nil;
	f->epoch = 0;
}

static int
openfid(Fid* fid, Frpc* fop)
{
	if (fid->fs && fid->fs != &ctlfs && fid->fs->fd >= 0){
		fop->f.type = Topen;
		fop->f.fid = fid->snr;
		fop->f.mode = fid->omode;
		return fsop(fid->fs, fop);
	} else
		return -1;
}

int
fidfree(Fid* fid, Frpc* fop)
{
	Fid *f, **l;
	int	nr;

	if (fid == nil)
		return 1;
	qlock(&fidlck);
	nfids--;
	l = &FIDHASH(fid->nr);
	for(f = *l; f; f = f->hnext) {
		if(fid == f) {
			*l = f->hnext;
			qunlock(&fidlck);
			closefid(f, fop);
			f->isopen = 0;
			if (f->mvol)
				mvoldelfid(f->mvol, f);
			putmvol(f->mvol, fop);
			putfs(f->fs);
			n_reset(f->sname);
			free(f->d);
			free(f->ureadbuf);
			qlock(&fidlck);
			f->hnext = freefids;
			freefids = f;
			nr = f->nr;
			Dbgprint(f, "fidfree: released fid=%d\n", nr);
			f->debug = 0;

for(f = *l; f != nil; f = f->hnext)
if (nr && f->nr == nr)fprint(2, "\n****\n**** BUG: dup fid in hash: %d\n****\n\n", nr);

			qunlock(&fidlck);
			return 1;
		}
		l = &f->hnext;
	}
	qunlock(&fidlck);
fprint(2, "\n\nXXX fidfree: not found? %X\n\n", fid);
	return 0;	
}

Fid *
fidalloc(int nr)
{
	Fid *new, **l;
	Name* sn;
	int	snr;

	qlock(&fidlck);
	l = &FIDHASH(nr);
	for(new = *l; new; new = new->hnext){
		// if nr is 0 it's a local fid used by this program
		// to issue requests to servers. We may have several
		// ones, and it's ok if they are dup.
		if(nr && new->nr == nr){
			qunlock(&fidlck);
			return 0;
		}
	}
	nfids++;
	if(freefids == 0) {
		nafids++;
		freefids = emalloc(sizeof(Fid));
		freefids->sname = n_new();
		freefids->hnext = nil;
		freefids->snr = ++fidgen;
	}

	new = freefids;
	freefids = new->hnext;

	sn = new->sname;
	snr= new->snr;
	memset(new, 0, sizeof(Fid));
	new->sname= sn;
	new->snr = snr;
	new->nr = nr;
	new->hnext = *l;
	*l = new;

	qunlock(&fidlck);
	return new;
}

void
dumpfids(void)
{
	int	i;
	Fid*	f;
	int	nh;

	fprint(2, "fids: %ld used %ld allocated\n", nfids, nafids);
	qlock(&fidlck);
	for (i = 0; i < nelem(fidhash); i++){
		nh = 0;
		for (f = fidhash[i]; f; f = f->hnext){
			nh++;
			fprint(2, "   %X\n", f);
			fprint(2, "       %W\n", f->mvol);
		}
		if (nh > 5)
			fprint(2, "\t[%d fids in bucket]\n", nh);
	}
	qunlock(&fidlck);
}

/*
 * Version, Auth, and Attach are handled by us. 
 * To the user, it seems that a volume is being mounted.
 * All auth is done by the factotum underlying this program, not by clients.
 */
static void
fsversion(Frpc* fop)
{
	if(strncmp(fop->f.version, "9P", 2) != 0){
		fop->f.version = "unknown";
	}
	fop->r.tag = fop->f.tag;
	fop->r.type = Rversion;
	fop->r.version = "9P2000";
	/*
	 * BUG: We don't know what the real file server msize will
	 * be, we tried using iounit in open, but the kernel seems
	 * to ignore that.
	 */
	if (fop->f.msize > 8*1024)
		fop->f.msize = 8*1024;
	msglen = fop->r.msize = fop->f.msize;
}

static void
fserror(Frpc* fop, char* e)
{
	fop->r.tag = fop->f.tag;
	fop->r.type = Rerror;
	fop->r.ename= e;
}

static void
fsauth(Frpc* fop)
{
	fserror(fop, Eauth);
}

static int
fidbound(Fid* fid, Vol* v)
{
	return !fid->stale && fid->fs &&
		v && v->fs == fid->fs && fid->epoch == fid->fs->epoch;
}

/* Fid may be a brand new one, or an already bound one.
 * If bindfid succeeds, fid will be a valid fid in v
 * for its sname in v->fs.
 * This does not reopen the fid if it was open.
 */
static int
bindfid(Fid* fid, Mvol* mv, Vol* v, Frpc* fop)
{
	int	unbound;

	if (fidbound(fid, v))
		return 1;
	closefid(fid, fop);
	if (mv == nil){	// fid into our ctl fs. We're done.
		incref(&ctlfs);
		putfs(fid->fs);
		fid->fs = &ctlfs;
		fid->epoch = ctlfs.epoch;
		fid->stale = 0;
		return 1;
	}
	if (fid->mvol == nil){
		assert(!fid->linked);
		unbound = 1;
		fid->mvol = mv;
		if (mv) {
			incref(mv);
			fid->notimeout = mv->notimeout;
			fid->debug = mv->debug;
		}
	} else {
		unbound = 0;
		assert(fid->mvol == mv);
	}
	if(v && v->fs)
		incref(v->fs);
	putfs(fid->fs);
	fid->fs = (v ? v->fs : nil);
	if (fid->fs != nil && fid->mvol != nil && fid->mvol->musthave)
		fid->fs->musthave = 1;
	fid->stale = 0;
	if (v && v->slash && v->fs){
		if (walkfid(v->slash, fid, nil, 0, fop) <= 0){
			if (unbound){
				decref(fid->mvol);
				fid->mvol = nil;
			}
			return 0;
		}
	} else {
		fid->qid.type = QTDIR;
		fid->qid.path = 0;
		fid->qid.vers = 0;
	}
	if (unbound && mv)
		mvoladdfid(mv, fid);
	return 1;
}

static void
fsattach(Frpc* fop)
{
	Vol*	v;
	Mvol*	mv;
	Fid*	fid;
	int	nr;
	int	tag;

	nr = fop->f.fid;
	tag= fop->f.tag;
	if (strcmp(getuser(), fop->f.uname)){
		fserror(fop, Euser);
		return;
	}
	mv = newmvol(fop->f.aname);
	if (mv == nil && fop->f.aname[0] != 0){
		fserror(fop, Espec);
		return;
	}
	updatemvol(mv, fop);
	fid = fidalloc(nr);
	if (fid == nil){
		putmvol(mv, fop);
		fop->r.tag = tag;
		fserror(fop, Ebadfid);
		return;
	}
	if (!mv){
		fid->qid.path = Ctldirqid;
		fid->qid.vers = 0;
		fid->qid.type = QTDIR;
	}
	for(;;){
		// If all vols fail, mv ends up empty.
		// the rpc is guaranteed to work there.
		v = getmvolvol(mv, 0);
		if (bindfid(fid, mv, v, fop))
			break;
		vdprint(2, "attach: deadvol: %s %s\n", v->addr, v->name);
		qlock(&volslck);
		deadvol(v, fop);
		qlock(mv);
		mvolunmount(mv, v, fop);
		qunlock(mv);
		qunlock(&volslck);
	}
	if (fid->fs == nil)
		newfsqid(&fid->qid);	// unique qid for fake fs /
	fop->r.type = Rattach;
	fop->r.qid = fid->qid;
	fop->r.tag = tag;
}

/* We must ensure the system always gets the same qid for
 * a file we serve:
 *	- The kernel can be using only those files that
 * 	have fids on us. Usually Chans for a name space.
 *	- User programs may remember qids for closed files,
 *	that do not have fids on us (e.g., acme for its files).
 * 
 * The qids we use are the first ones seen, decorated
 * in the high long in path with nfid->fs->qid. To make them unique.
 * The decoration ensures that fids forgotten get the same qid if
 * their file stays the same.
 * We leave Qid.vers as the real qid.vers+qid.path, to report changes
 * for files whose path/vers changes.
 */
static void
fixseenqid(Fid* nfid, Fs* fs, Qid* wqid, int i)
{
	Fid*	old;
	ulong	vers;

	vers = wqid[i].vers + (ulong)wqid[i].path;

	old = mvolgetfid(nfid->mvol, nfid->sname, nfid);
	if (old){
		nfid->qid = old->qid;
	} else {
		nfid->qid = wqid[i];
		if (fs)
			nfid->qid.path |= fs->qid.path;
	}
	nfid->qid.vers = vers;
	wqid[i] = nfid->qid;
	wqid[i].vers = vers;
}


/* Walks (the unbound) nfid through elems in nfid->sname.
 * The start for the walk is fid.
 * The sname in nfid is updated to reflect the walk.
 * Qids in nfid is updated to: <fsqid>|<realqid>
 * Qids in fop->r.wqid[] are updated as well.
 *
 * When elems is nil, we walk nfid->sname, and leave the sname
 * as it was. This is used while rebinding a fid into a new volume.
 *
 * Return 1 if could walk elems; 0 if couldn't; -1 if got an IO error.
 */
int
walkfid(Fid* fid, Fid* nfid, char** elems, int nelems, Frpc* fop)
{
	int	i;
	int	rebind;
	int	ndotdot;
	int	a,b;
	int	r;

	rebind = ndotdot = 0;
	if (elems == nil){
		rebind = 1;
		elems = nfid->sname->elems;
		nelems= nfid->sname->nelems;
	} else {
		/* Drop the part of "../../..." that might
		 * go past the root in the volume. We assume
		 * that we won't get "a/../../..."
		 */
		for (i = 0; i < nelems; i++)
			if (strcmp(elems[i], ".."))
				break;
			else
				ndotdot++;
		while(ndotdot > fid->sname->nelems){
			elems++;
			nelems--;
			ndotdot--;
		}
	}
	fop->f.type = Twalk;
	fop->f.fid = fid->snr;
	fop->f.newfid = nfid->snr;
	fop->f.nwname = nelems;
	for (i = 0; i < nelems; i++)
		fop->f.wname[i] = elems[i];
	r = fsop(fid->fs, fop);
	if (r < 0 && !strncmp(fop->r.ename, "io ", 3)){
		dbgprint(fid, "walkfid: io err: %X\n", fid);
		return r;
	}
	if (nfid != fid && fid->fs)
		nfid->epoch = fid->fs->epoch;
	if (!rebind && fop->r.type == Rwalk && nelems == 0 && fid != nfid){
		n_reset(nfid->sname);
		n_cat(nfid->sname, fid->sname);
	}
	if (fop->r.type == Rwalk && fop->r.nwqid > 0){
		if (!rebind){
			/* This is in the inner loop.
			 * If this is improved, the user will notice.
			 */
			if (fid != nfid){
				n_reset(nfid->sname);
				n_cat(nfid->sname, fid->sname);
			}
			if (fop->r.nwqid < nelems)
				n_getpos(nfid->sname, &a, &b);
			for (i = 0; i < nelems; i++){
				if (!strcmp(elems[i], ".."))
					n_dotdot(nfid->sname);
				else
					n_append(nfid->sname, elems[i]);
				fixseenqid(nfid, fid->fs, fop->r.wqid, i);
			}
			Dbgprint(fid, "\tfixed fid: %X\n", nfid);
			if (fop->r.nwqid < nelems)
				n_setpos(nfid->sname, a, b);
		}
	}
	if (nelems == 0 && nfid != fid && fop->r.nwqid == nelems){
		// nfid->qid = fid->qid;
		fixseenqid(nfid, fid->fs, &fid->qid, 0);
	}
	return fop->r.nwqid == nelems;
}


static Ref once;

static void
fswalk(Frpc* fop)
{
	Fid*	fid;
	Fid*	nfid;
	Vol*	v;
	Frpc*	fwop;	// Auxiliary used to keep fop untouched.
	int	r, nvol;
	int	isunion;
	int	musthave;
	int	d, vd;

	fid = fop->fid = getfid(fop->f.fid);
	if (fid == nil || fid->isopen){
		fserror(fop, Ebadfid);
		return;
	}
	incref(&once);
	if (once.ref == 1 && hdebug){
		d = debug;
		vd= vdebug;
	} else
		d = vd = -1;

	dbgprint(fid, "<- %F\n", &fop->f);
	Dbgprint(fid, "Walk %X\n", fid);
	qlock(fid);
	if(fop->f.newfid != fop->f.fid){
		nfid = fidalloc(fop->f.newfid);
		if(!nfid) {
			fprint(2, "BUG: dup fid\n");
			/* Try to recover.
			 * The user already knows this happens.
			 */
			nfid = getfid(fop->f.newfid);
			assert(nfid);
			nfid->nr = 0; // forget it.
			nfid = fidalloc(fop->f.newfid);
			if (!nfid){
				fprint(2, "BUG at BUG\n");
				fserror(fop, Edupfid);
				qunlock(fid);
				decref(&once);
				return;
			}
		}
		assert(!nfid->linked);
		assert(nfid->mvol == nil);
		if (nfid->mvol = fid->mvol){
			incref(fid->mvol);
			mvoladdfid(nfid->mvol, nfid);
			nfid->notimeout = fid->mvol->notimeout;
			nfid->debug = fid->mvol->debug;
		}
	} else
		nfid = fid;
	fserror(fop, Ebadfid);	// by default
	fwop = rpcalloc();
	fwop->f.tag = fop->f.tag;
	nvol = 0;
	isunion = (fid->mvol ? fid->mvol->isunion : 0);
	musthave= (fid->mvol ? fid->mvol->musthave : 0);
	do {
		/* Try the walk in the volume used.
		 * When all volumes in the mvol fail,
		 * an empty fs is used intead of the real thing.
		 * For musthave mvols, we don't accept the empty one.
		 */
		r = -1;
		updatemvol(fid->mvol, fwop);
		v = getmvolvol(fid->mvol, nvol++);
		if (!bindfid(fid, fid->mvol, v, fwop))
			continue;
		if (musthave && (v == nil || fid->fs == nil)){
			fprint(2, "wait for %s\n", fid->mvol->name);
			if (d != -1 && vd != -1)
				debug = vdebug = 2;
			sleep(Tmout*1000);
			checkvols(fwop);
			nvol = 0;
			continue;
		}
		r = walkfid(fid, nfid, fop->f.wname, fop->f.nwname, fop);
		if (r > 0){
			if (nfid != fid && nfid->fs != fid->fs){
				putfs(nfid->fs);
				if (nfid->fs = fid->fs)
					incref(nfid->fs);
			}
		}
	} while ((r <= 0 && v && isunion) || (r <  0 && musthave));
	if (d != -1 && vd != -1){
		debug = d;
		vdebug = vd;
	}
	decref(&once);
	rpcfree(fwop);
	if (r <= 0 && nfid != fid)	// couln't walk. Get rid of it.
		fop->freefid = nfid;

	free(nfid->d);
	nfid->d = nil;
	Dbgprint(fid, "Walked fid %X\nWalked nfid %X\n", fid, nfid);
	qunlock(fid);
}


/* To read all entries of a union at once.
 * We know that fid points to the root of a union.
 */
static	void
fidreadall(Fid *fid, Frpc* fop)
{
	char*	buf;
	long	bufsz;
	long	buflen, off;
	long	nr;
	Vol*	v;
	int	nvol;

	buf = emalloc(16*1024);
	bufsz = 16*1024;
	buflen = 0;

	updatemvol(fid->mvol, fop);
	nvol = 0;
	do {
		v = getmvolvol(fid->mvol, nvol++);
		if (v == nil && buflen > 0)
			break;
		closefid(fid, fop);
		if (!bindfid(fid, fid->mvol, v, fop)) // ignore
			continue;
		nr = openfid(fid, fop);
		off = 0;
		if (nr >= 0){
			for(;;){
				if (buflen == bufsz){
					buf = erealloc(buf, bufsz + 16*1024);
					bufsz += 16*1024;
				}
				fop->f.type = Tread;
				fop->f.count= bufsz - buflen;
				if (fid->iounit && fop->f.count>fid->iounit)
					fop->f.count = fid->iounit;
				fop->f.offset = off;
				nr = fsop(fid->fs, fop);
				if (nr < 0 || fop->r.count <= 0)
					break;	// next volume
				memmove(buf+buflen, fop->r.data, fop->r.count);
				buflen += fop->r.count;
				off += fop->r.count;
			}
			//closefid(fid, fop);
		}
	} while (v);
	fid->ureadbuf = buf;
	fid->ureadlen = buflen;
}

static int
mayretry(Frpc* fop)
{
	if (fop->f.type == Tstat || fop->f.type == Tread)
		return 1;
	if (fop->f.type == Topen && fop->f.mode == OREAD)
		return 1;
	return 0;
}

/* Similar to fsop(fid, fop), but retries on other
 * volumes available for the fid's mvol on io errors.
 * Note that this always retries in the first vol.
 */
static void
mvolop(Fid* fid, Frpc* fop)
{
	Frpc*	fwop;
	Vol*	v;
	int	r;
	int	ntries;

	fwop = nil;
	ntries = 0;
again:
	if (fid->stale || (fid->fs && fid->fs->fd < 0)){
		if (fwop == nil)
			fwop = rpcalloc();
		fwop->f.tag = fop->f.tag;
		closefid(fid, fwop);
	}
	/* Recover fids for volumes that were gone.
	 * On unions we don't recover. The particular vol was gone.
	 */
	if (fid->fs == nil && !fid->mvol->isunion && mayretry(fop)){
		dbgprint(fid, "rebind for RPC %d: %X\n", fop->f.type, fid);
		if (fwop == nil)
			fwop = rpcalloc();
		fwop->f.tag = fop->f.tag;
		for(;;){
			fwop->fid = fid;
			updatemvol(fid->mvol, fwop);
			v = getmvolvol(fid->mvol, 0);
			if(v || !fid->mvol || !fid->mvol->musthave)
				break;
			sleep(Tmout*1000);
			checkvols(fwop);
			dprint(2, "waiting for %s\n", fid->mvol->name);
		}
		bindfid(fid, fid->mvol, v, fwop);
		if (fid->isopen)
			openfid(fid, fwop);
	}

	r = fsop(fid->fs, fop); 

	/* Retry if necessary.
	 */
	if (r < 0){
		fserror(fop, fop->err);
		if (!fid->mvol->isunion)
		if (!strncmp(fop->r.ename, "io ", 3) && mayretry(fop)){
			if (fwop == nil)
				fwop = rpcalloc();
			fwop->f.tag = fop->f.tag;
			checkvols(fwop);
			ntries++;
			if (ntries > 2)
				fprint(2, "%d tries for fid %X\n", ntries, fid);
			goto again;
		}
	}
	rpcfree(fwop);
}

// Does not fix qids for dir reads.
// Not needed to preserve client's binds.
static void
fsread(Frpc* fop)
{
	long	off, len, n, count;
	Dir	d;
	Fid*	fid;

	fid = fop->fid = getfid(fop->f.fid);
	if (fid == nil){
		fserror(fop, Ebadfid);
		return;
	}
	dbgprint(fid, "<- %F\n", &fop->f);
	if (!fid->isopen){
		fserror(fop, Enotopen);
		return;
	}
	qlock(fid);
	fop->f.fid = fid->snr;
	if (fop->f.count > fid->iounit && fid->iounit)
		fop->f.count = fid->iounit;
	off = fop->f.offset;
	count = fop->f.count;
	if (fid->sname->nelems > 0 || !fid->mvol || !fid->mvol->isunion){
		mvolop(fid, fop);
		qunlock(fid);
		return;
	}
	/* Volume union reads
	 */
	if (fid->ureadbuf == nil){
		fidreadall(fid, fop);
		fop->f.type = Tread;
	}
	fop->r.tag = fop->f.tag;
	fop->r.type= Rread;
	fop->r.count = 0;
	fop->r.data = (char*)fop->buf;
	len = fid->ureadlen;
	while(count > 0 && off < len){
		n = convM2D((uchar*)fid->ureadbuf + off, len - off, &d, nil);
		if (n <= 0 || n > count)
			break;
		memmove(fop->r.data + fop->r.count, fid->ureadbuf + off, n);
		off += n;
		count -= n;
		fop->r.count += n;
	}
	qunlock(fid);
}

static void
fsfwd(Frpc* fop)
{
	Fid*	fid;
	int	op;

	fid = fop->fid = getfid(fop->f.fid);
	if (fid == nil){
		fserror(fop, Ebadfid);
		return;
	}
	dbgprint(fid, "<- %F\n", &fop->f);
	op = fop->f.type;
	qlock(fid);
	fop->f.fid = fid->snr;
	if (fop->f.type == Twrite && fid->iounit && fid->iounit < fop->f.count)
		fop->f.count = fid->iounit;
	mvolop(fid, fop);
	switch(fop->r.type){
	case Ropen:
		fop->r.qid = fid->qid;	// comes from walkfid
	open:
		fid->isopen = 1;
		fid->omode = fop->f.mode;
		if (fop->r.iounit > msglen)
			fop->r.iounit = msglen;
		fid->iounit = fop->r.iounit;
		break;
	case Rremove:
		mvoldelotherfids(fid->mvol, fid);
		break;
	case Rcreate:
		n_append(fid->sname, fop->f.name);
		mvoldelotherfids(fid->mvol, fid);
		fixseenqid(fid, fid->fs, &fop->r.qid, 0);
		goto open;
	case Rstat:
		if (fop->d && fid->fs)
			fixseenqid(fid, fid->fs, &(fop->d->qid), 0);
		break;
	}
	qunlock(fid);
	if (op == Tremove || op == Tclunk){
		putfs(fid->fs);
		fid->fs = nil;
		fop->freefid = fid;
	}
}

static void
fsflush(Frpc* fop)
{
	Fid*	fid;

	fop->r.tag = fop->f.tag;
	fop->r.type= Rflush;
	fid = fop->fid = getfid(fop->f.fid);
	if (fid == nil)
		return;
	dbgprint(fid, "<- %F\m", &fop->f);
	/* RACE: We don't lock the fid.
	 * The flushed request is probably holding the fid
	 * lock, and that would block the flush.
	 */
	fop->f.fid = fid->snr;
	if (!fid->stale && fid->fs && fid->fs->fd >= 0)
		fsop(fid->fs, fop);
}

void (*fscalls[Tmax])(Frpc*) =
{
	[Tversion]	fsversion,
	[Tauth]		fsauth,
	[Tattach]	fsattach,
	[Tflush]	fsflush,
	[Twalk]		fswalk,
	[Tread]		fsread,
	[Topen]		fsfwd,
	[Tcreate]	fsfwd,
	[Tclunk]	fsfwd,
	[Twrite]	fsfwd,
	[Tremove]	fsfwd,
	[Tstat]		fsfwd,
	[Twstat]	fsfwd,
};
 
