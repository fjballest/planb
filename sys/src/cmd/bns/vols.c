/*
 * Volumes and mount points for volumes.
 * Support for the multiplexor.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <bio.h>
#include "names.h"
#include "vols.h"

enum {
	Worst = 10000,	// any big number is a bad m->choice.
};

QLock	volslck;
Vol**	vols;
int	nvols;

QLock	mvolslck;
Mvol**	mvols;
int	nmvols;

Ref	epoch;

static int
diedvol(Vol* v)
{
	int	dead;

	if (v->disabled>0 || (v->fs && v->fs->fd < 0)){
		vdprint(2, "checkvols: dead vol: %s, %s\n", v->addr, v->name);
		dead = 1;
	} else if (v->fs && v->fs->epoch > v->epoch){
		vdprint(2, "checkvols: gone vol: %s, %s\n", v->addr, v->name);
		dead = 1;
	} else
		dead = 0;
	return dead;
}

static void
volunmount(Vol* v, Frpc* fop, int dead)
{
	if (dead && v->dead)
		return;

assert(v->ref == 0 || !canqlock(&volslck));

	if (dead && !diedvol(v)){
		// the vol resurrected while we were waiting
		// for the lock. In any case, it's no longer dead.
		return;
	}
	v->dead |= dead;
	if (v->slash){
		fidfree(v->slash, fop);
		v->slash = nil;
	}
	if (!v->dead){
		putfs(v->fs);
		v->fs = nil;
	}
	incref(&epoch);
	v->epoch = epoch.ref;
	vdprint(2, "unmounted %s %s e=%ld\n", v->addr, v->name, epoch.ref);
}

void
deadvol(Vol* v, Frpc* fop)
{
	volunmount(v, fop, 1);
}

static void
putvol(Vol* v, Frpc* fop)
{
	if (v && decref(v) == 0)
		volunmount(v, fop, 0);
}

static void
addmvol(Mvol* v)
{
	int	i;

	qlock(&mvolslck);
	for (i = 0; i < nmvols; i++)
		if (mvols[i] == nil)
			break;
	if (i == nmvols){
		if (!(nmvols%16))
			mvols = erealloc(mvols, sizeof(Mvol*)*(nmvols+16));
		nmvols++;
	}
	mvols[i] = v;
	qunlock(&mvolslck);
}

static void
delmvol(Mvol* v)
{
	int	i;

	qlock(&mvolslck);
	for (i = 0; i < nmvols; i++)
		if (mvols[i] == v){
			mvols[i] = nil;
			qunlock(&mvolslck);
			return;
		}
	fprint(2, "bad mvol\n");
	abort();
	qunlock(&mvolslck);
}

void
tracemvol(char* name, int dbg)
{
	Fid*	f;
	int	i;

	qlock(&mvolslck);
	for (i = 0; i < nmvols; i++)
		if (name == nil || !strcmp(mvols[i]->name, name)){
			mvols[i]->debug = dbg;
			for (f = mvols[i]->fids; f ; f = f->mnext)
				f->debug = dbg;
		}
	qunlock(&mvolslck);
}

Mvol*
newmvol(char* spec)
{
	Mvol*	m;
	char*	c;

	if (!*spec)		// ctl file system. No mvols there
		return nil;	// this is not an error.

	if (!strchr("*/!-", spec[0]))
		return nil;
	m = emalloc(sizeof(Mvol));
	memset(m, 0, sizeof(Mvol));
	m->spec = estrdup(spec);
	switch(spec[0]){
	case '!':
		m->musthave = 1;
		spec++;
		break;
	case '*':
		m->isunion = 1;
		spec++;
		break;
	case '-':
		c = ++spec;
		spec = strchr(spec, ' ');
		if (spec == nil){
			free(m->spec);
			free(m);
			return nil;
		}
		*spec++ = 0;
		if (strchr(c, 'M'))
			m->musthave = 1;
		if (strchr(c, 'U'))
			m->isunion = 1;
		if (strchr(c, 'T'))
			m->notimeout = 1;
		break;
	}
	c = strpbrk(spec, " \t");
	parseexpr(c, &m->cnstr);
	if (c)
		*c = 0;
	m->name = estrdup(spec);
	if (!m->isunion)
		m->choice = Worst;
	m->ref = 0;	// Nobody *using* us. Caller will bindfid
	addmvol(m);
	return m;
}

static void
mvolfree(Mvol* m, Frpc* fop)
{
	int	i;
	Fid*	f;

	delmvol(m);
	free(m->name);
	free(m->spec);
	for (i = 0; i < m->nvols; i++)
		putvol(m->vols[i], fop);
	free(m->vols);
	for (f = m->fids; f; f = f->mnext)
		fprint(2, "mvolfree: %p has linked fid %X\n", m, f);
	free(m);
}

Fid*
mvolgetfid(Mvol* m, Name* sname, Fid* excl)
{
	Fid*	f;

	if (!m)
		return nil;
	qlock(m);
	for (f = m->fids; f ; f = f->mnext)
		if (f != excl && n_eq(f->sname, sname))
			break;
	qunlock(m);
	return f;
}

void
mvoldelotherfids(Mvol* m, Fid* fid)
{
	Fid**	l;
	Fid*	f;

	if (!m || !fid)
		return;
	qlock(m);
again:
	for (l = &m->fids; f = *l ; l = &f->mnext){
		if (f != fid && n_eq(f->sname, fid->sname)){
			*l = f->mnext;
			f->mnext = nil;
			f->linked = 0;
			goto again;
		}
	}
	qunlock(m);
}

void
mvoladdfid(Mvol* m, Fid* fid)
{
	Fid**	l;
	Fid*	f;

	assert(!fid->linked);
	assert(fid->mnext == nil);
	qlock(m);
	for (l = &m->fids; f = *l ; l = &f->mnext){
		if (f == fid)
			fprint(2, "dup: pc = %lx: %X\n", getcallerpc(&m), f);
		assert(f != fid);
		if (n_eq(f->sname, fid->sname)){
			fid->qid = f->qid;
		}
	}
	assert(l);
	*l = fid;
	fid->linked = 1;
	qunlock(m);
}

void
mvoldelfid(Mvol* m, Fid* fid)
{
	Fid**	l;
	Fid*	f;

	if (!fid->linked)
		return;
	qlock(m);
	for (l = &m->fids; f = *l ; l = &f->mnext){
		if (f == fid){
			*l = f->mnext;
			f->mnext = nil;
			fid->linked = 0;
			break;
		}
	}
	qunlock(m);
	assert(f != nil);
}

void
putmvol(Mvol* m, Frpc* fop)
{
	if (m && decref(m) == 0)
		mvolfree(m, fop);
}

static int
volmatch(Vol* v, Mvol* m)
{
	if (strcmp(v->name, m->name))
		return -1;
	return exprmatch(&v->ccnstr, &m->cnstr);
}

static int
hasvol(Mvol* m, Vol* v)
{
	int	i;

	for (i = 0; i < m->nvols; i++)
		if (m->vols[i] == v)
			return 1;
	return 0;
}

static int
mvolmount(Mvol* m, Vol* v, Frpc* fop)
{
	if (!v->fs)
		if (!volmount(v, fop))
			return 0;
	assert(v->fs);
	if (!(m->nvols%16))
		m->vols = erealloc(m->vols, (m->nvols+16)*sizeof(Vol*));
	incref(v);
	m->vols[m->nvols] = v;
	m->nvols++;
	return 1;
}

static void
unbindfids(Mvol* m, Fs* fs)
{
	Fid*	f;

	for (f = m->fids; f ; f = f->mnext){
		if (f->fs == fs)
			f->stale = 1;
	}
}

void
mvolunmount(Mvol* m, Vol* v, Frpc* fop)
{
	int	i;
	int	newi;

	unbindfids(m, v->fs);

	newi = -1;
	for (i = 0; i < m->nvols; i++)
		if (newi >= 0)
			m->vols[newi++] = m->vols[i];
		else if (m->vols[i] == v){
			putvol(v, fop);
			newi = i;
		}
	if (newi >= 0)
		m->nvols--;
}

Vol*
getmvolvol(Mvol* m, int i)
{
	Vol*	v;

	v = nil;
	if (m != nil){
		qlock(m);
		if (i < m->nvols)
			v = m->vols[i];
		qunlock(m);
	}
	return v;
}

int
volmount(Vol* v, Frpc* fop)
{ 
	int	isrecover;
	Fid*	vfid;

	if (v->disabled > 0)
		return 0;

assert(!canqlock(&volslck));

	if (!v->dead && v->fs && v->fs->fd >= 0 && v->fs->epoch <= v->epoch){
		// already mounted.
		return 1;
	}
	isrecover = (v->fs != nil);
	assert(v->slash == nil);
	if (v->fs == nil)
		v->fs = getfs(v->addr, v->spec);
	if (v->fs && v->fs->lat < Badlatency)
		cnstrcat(&v->ccnstr, &netokcnstr);
	else
		cnstrcat(&v->ccnstr, &netbadcnstr);
	vfid = fidalloc(0);
	incref(v->fs);
	vfid->fs = v->fs;
	assert(v->fs->fid);
	if (walkfid(v->fs->fid, vfid, v->sname->elems, v->sname->nelems, fop)<=0){
		v->dead = 1;
		incref(&epoch);
		v->epoch = epoch.ref;
		fidfree(vfid, fop);
		vdprint(2, "vol %s!%s: not there\n", v->addr, v->name);
	} else {
		n_reset(vfid->sname);	// it is "/", by convention.
		v->slash = vfid;
		v->dead = 0;
		if (isrecover)
			incref(&epoch);
		v->epoch = epoch.ref;
		vdprint(2, "vol %s!%s mounted e=%ld fse=%ld\n", v->addr, v->name,
			v->epoch, v->fs->epoch);
	}
	return !v->dead;
}


/* Checks out volumes for death and resurection.
 */
int
checkvols(Frpc* fop)
{
	static long	lepoch;
	int	i;
	Vol*	v;
	int	some;

	if (0 && lepoch == epoch.ref)
		return 0;
	some = 0;
	qlock(&volslck);
	for(i =0; i < nvols; i++){
		v = vols[i];
		// Vols with dead a fs are dead.
		// Same for disabled vols (although their fss are kept)
		if (!v->dead){
			if (diedvol(v)){
				some++;
				vdprint(2, "checkvols: dead vol: "
					"%s %s\n", v->addr, v->name);
				deadvol(v, fop);
			} 
		}
		// Vols with recovered fs are no longer dead
		if (v->dead && !v->disabled){
			if (v->fs && v->fs->fd >= 0){
				some++;
				vdprint(2, "checkvols: came vol: "
					"%s %s\n", v->addr, v->name);
				volmount(v, fop);
			}
		}
	}
	lepoch = epoch.ref;
	qunlock(&volslck);
	return some;
}


/* vols are locked. m is locked
 */
static void
unmountdeadvols(Mvol* m, Frpc* fop)
{
	int	i;
	Vol*	v;

again:
	for (i = 0; i < m->nvols; i++){
		v = m->vols[i];
		if (v->dead || v->epoch > m->epoch){
			vdprint(2, "\t%p: unmounted: %V\n", m, v);
			mvolunmount(m, v, fop);
			if (!m->isunion){
				/* If we loose a vol, and have many ones we
				 * must try any other one that suffices.
				 * This ensures we try all volumes.
				 */
				m->choice = Worst; // Any big number
				m->epoch = 0;
			}
			goto again;
		}
	}
}

/* vols are locked.
 * m is locked
 */
static void
mountnewvols(Mvol* m, Frpc* fop)
{
	int	i;
	int	nr;
	Vol*	v;

	if (!m->isunion && m->nvols > 0 && m->choice == 0){
		// Not a union and already got the preferred one.
		return;
	}
	for(i = 0; i < nvols; i++){
		if (vols[i]->dead || vols[i]->epoch <= m->epoch)
			continue;
		nr = volmatch(vols[i], m);
		if (nr >= 0)
			vdprint(2, "\t%p: match: %s %d\n", m,vols[i]->addr, nr);
		if (nr < 0 || (!m->isunion && nr >= m->choice))
			continue;
		if (hasvol(m, vols[i]))
			continue;
		if (m->nvols)
			v = m->vols[0];
		else
			v = nil;
		if (mvolmount(m, vols[i], fop)){
			vdprint(2, "\t%p: mounted: %V\n", m, vols[i]);
			if (!m->isunion){
				if (v != nil)
					mvolunmount(m, v, fop); // old preferred
				m->choice = nr;
			}
		}
		if (!m->isunion && m->choice == 0)
			break;
	}
}

void
updatemvol(Mvol* m, Frpc* fop)
{
	if (m == nil || m->epoch == epoch.ref)
		return;

	vdprint(2, "update mvol %p %s me=%ld e=%ld\n",
		m, m->name, m->epoch, epoch.ref);
	qlock(&volslck);
	qlock(m);
	unmountdeadvols(m, fop);
	mountnewvols(m, fop);
	m->epoch = epoch.ref;
	qunlock(m);
	qunlock(&volslck);
}

static char*
fstr(char* s)
{
	if (s == nil)
		return "-";
	else
		return s;
}

int
Vfmt(Fmt* f)
{
	Vol*	v;
	char*	addr;
	char*	spec;
	v = va_arg(f->args, Vol*);
	if (!strcmp(v->addr, "#s/boot"))
		addr = "-";
	else
		addr = v->addr;
	if (v->spec == nil || *v->spec == 0)
		spec = "-";
	else
		spec = v->spec;
	return fmtprint(f, "%s\t%s\t%N\t%s\t%k",
			addr, fstr(spec), v->sname, v->name, &v->ccnstr);
}

int
Wfmt(Fmt* f)
{
	Mvol*	v;
	char buf[120];
	char*	s;
	int	i;
	char*	u;

	v = va_arg(f->args, Mvol*);
	if (v == nil)
		return fmtprint(f, "nil");
	s = buf;
	buf[0] = 0;
	s = seprint(s, buf+120, " ");
	for (i = 0; i < v->nvols; i++)
		s= seprint(s, buf+120, "%s!%s ", v->vols[i]->addr, v->vols[i]->name);
	if (v->isunion)
		u = "union";
	else if (v->musthave)
		u = "musthave";
	else
		u = "";
	return fmtprint(f, "%s\t%K\t[%s] %s", v->name, &v->cnstr, buf, u);
}

void
dumpvols(void)
{
	int	i;

	fprint(2, "vols: epoch=%ld\n", epoch.ref);
	qlock(&volslck);
	for (i = 0; i < nvols; i++){
		fprint(2, "%p %V ref=%ld e=%ld fs=%p",
			vols[i], vols[i],
			vols[i]->ref, vols[i]->epoch, vols[i]->fs);
		if (vols[i]->disabled>0 && !vols[i]->dead)
			fprint(2, " dying");
		else if (vols[i]->dead)
			fprint(2, " dead");
		fprint(2, "\n");
		if (vols[i]->slash)
			fprint(2, "\t\tslash=%X\n", vols[i]->slash);
		sleep(300);	// give time to kprint drain queue
	}
	qunlock(&volslck);
	fprint(2, "\n");
}

void
dumpmvols(void)
{
	int	i;
	Fid*	f;

	fprint(2, "mvols:\n");
	qlock(&mvolslck);
	for (i = 0; i < nmvols; i++)
		if (mvols[i]){
			qlock(mvols[i]);
			fprint(2, "   %p %W\te=%ld ref=%ld choice=%d\n",
				mvols[i], mvols[i],
				mvols[i]->epoch, mvols[i]->ref, mvols[i]->choice);
			for (f = mvols[i]->fids; f; f = f->mnext)
				fprint(2, "      %X\n", f);
			qunlock(mvols[i]);
			/* Give time to /dev/kprint so it could make
			 * more room and we see it all
			 */
			sleep(200);
		}
	qunlock(&mvolslck);
			
}


static char*
addrhost(char* addr)
{
	char* na;
	char*	p;

	na = netmkaddr(addr, "tcp", "9p");
	// na is now xxx!xxx!xxx
	na = strchr(na, '!') + 1;
	p = strchr(na, '!');
	*p = 0;
	if (!strcmp(na, "localhost") || !strcmp(na, "*"))
		na = sysname();
	return estrdup(na);
}

/*
 * Volumes are allocated but not freed.
 * A gone volume has v->dead set.
 */
static Vol*
newvol(char* cfname, char* addr, char* spec, char* path, char* name, char* cnstr)
{
	Vol*	v;
	char*	c;
	char*	s;
	char*	elems[16];
	int	nelems, i;
	int	local;

	v = emalloc(sizeof(Vol));
	memset(v, 0, sizeof(Vol));
	v->cfname = estrdup(cfname);
	v->addr = estrdup(addr);
	local = (v->addr[0] == '/' || v->addr[0] == '#');
	if (local)
		v->host = estrdup(sysname());
	else {
		v->host = addrhost(addr);
	}
	local = !strcmp(v->host, sysname());
	if (!strcmp(spec, "-"))
		spec = "";
	v->spec = estrdup(spec);
	s =  estrdup(path);
	nelems = gettokens(s, elems, nelem(elems), "/");
	v->sname = n_new();
	for (i = 0; i < nelems; i++)
		n_append(v->sname, elems[i]);
	free(s);
	v->name = estrdup(name);
	v->cnstr = unquotestrdup(cnstr ? cnstr : "type=dir");
	v->epoch = epoch.ref;
	if (!strstr(v->cnstr, "sys=")){
		c = smprint("%s sys=%s", v->cnstr, v->host);
		assert(c);
		free(v->cnstr);
		v->cnstr = c;
	}
	if (local && !strstr(v->cnstr, "user=")){
		c = smprint("%s user=%s", v->cnstr, getuser());
		assert(c);
		free(v->cnstr);
		v->cnstr = c;
	}
	c = smprint("%s net=ok", v->cnstr);
	assert(c);
	free(v->cnstr);
	v->cnstr = c;
	parsecnstr(v->cnstr, &v->ccnstr);

	/* This counts how many mvols are using it.
	 * none so far.
	 */
	v->ref = 0;
	return v;
}

static void
addvol(char* cfname, char* addr, char* spec, char* path, char* name, char* cnstr)
{
	Vol*	v;
	int	i;

	qlock(&volslck);
	// ignore what we already know
	for (i = 0; i < nvols; i++){
		if (!strcmp(vols[i]->addr, addr) && !strcmp(vols[i]->name, name)){
			if (vols[i]->disabled){
				vols[i]->disabled = 0;
				// BUG: should do this:
				// parsecnstr(ncnstr, &cncnstr);
				// cnstrcat(&vols[i]->ccnstr, &cncnstr);
			}
			goto done;
		}
	}
	incref(&epoch);
	v = newvol(cfname, addr, spec, path, name, cnstr);
	vdprint(2, "new vol %s %s\n", v->addr, v->name);
	if (v != nil){
		if (!(nvols%16))
			vols = erealloc(vols, (nvols+16)*sizeof(Vol*));
		vols[nvols++] = v;
	}
done:
	qunlock(&volslck);
}


/* add/del name
 * add/del name 'cnstr'
 * add/del * 'cnstr'
 * set name 'cnstr' 'new cnstr' (used to update attributes)
 */
static void
volcmd(char* cmd, char* name, char* cnstr, char* ncnstr)
{
	static Cnstr	ccnstr;
	static Cnstr	cncnstr;
	int	i;
	Vol*	v;

	if (!cnstr)
		cnstr = "";
	if (!ncnstr)
		ncnstr = "";
	if (!strcmp(name, "*") && !strcmp(cnstr, "")){
		fprint(2, "%s: won't cmd all vols at once\n", argv0);
		return;
	}
	vdprint(2, "%s %s!%s\n", cmd, name, cnstr);
	parsecnstr(cnstr, &ccnstr);

	qlock(&volslck);
	for (i = 0; i < nvols; i++){
		v = vols[i];
		if (!strcmp(name, "*") || !strcmp(vols[i]->name, name))
		if (cnstrmatch(&v->ccnstr, &ccnstr)){
			if (!strcmp(cmd, "add"))
				vols[i]->disabled--;
			else if (!strcmp(cmd, "del"))
				vols[i]->disabled++;
			else if (!strcmp(cmd, "set")){
				parsecnstr(ncnstr, &cncnstr);
				cnstrcat(&vols[i]->ccnstr, &cncnstr);
			}
			vdprint(2, "%s %V\n", cmd, vols[i]);
			incref(&epoch);
			vols[i]->epoch = epoch.ref;
		}
	}
	qunlock(&volslck);
}

/* add name [ cnstr  ]
 * del name [ cnstr  ]
 * set name cnstr ncnstr
 * config line:
 * 	addr spec path name [cnstr]
 *	addr name [cnstr]
 * defaults:
 *	spec:main/active path:/ cnstr: 'user=$user host=$sysname loc=$location'
 */

void
cmdline(char* ln, char* fname, int lno)
{
	char*	p;
	int	nargs;
	char*	args[10];

	if (ln[0] == '#')
		return;
	p = strchr(ln, '\n');
	if (p)
		*p = 0;
	memset(args, 0, sizeof(args));
	nargs = tokenize(ln, args, nelem(args));
	if (!nargs)
		return;
	if (!strcmp(args[0], "del") || !strcmp(args[0], "add")){
		if (nargs == 2 || nargs == 3){
			volcmd(args[0], args[1], args[2], nil);
			return;
		}
	} else if (!strcmp(args[0], "set")){
		if (nargs == 4){
			volcmd(args[0], args[1], args[2], args[3]);
			return;
		}
	} else if (nargs == 2 || nargs == 3){
		// short config: addr name [cnstr]
		addvol(fname, args[0], "", "/", args[1], args[2]);
		return;
	} else if (nargs == 4 || nargs == 5){
		// long config: addr spec path name [cnstr]
		addvol(fname, args[0], args[1], args[2], args[3], args[4]);
		return;
	}
	fprint(2, "%s:%d: bad number of fields\n", fname, lno);
}

static Biobuf	bcfg;

void
fdconfig(int fd, char* name)
{
	char*	ln;
	int	lno;

	/* BUG: Gone volumes.
	 * when called from discover(), this should look out for
	 * vols not mentioned in the lines obtained below.
	 * Those volumes are gone as far as the adsrv can tell.
	 * For vols known from this adsrv, that are no longer known
	 * by such adsrv, issue a "del" to disable them.
	 * Also, beware that if they are later seen, addvol() must
	 * reuse the old volume, but reset its parameters.
	 */
	Binit(&bcfg, fd, OREAD);
	for(lno = 1; ln = Brdstr(&bcfg, '\n', 1); lno++){
		if (ln[0] == 0)
			break;
		cmdline(ln, name, lno);
		free(ln);
	}
	Bterm(&bcfg);
}

static void
stdconfig(void)
{
	char*	fs;
	char*	spec;
	char*	uspec;
	char	cfg[128];

	fs = getenv("fs");
	if (fs == nil)
		fs = strdup("193.147.71.86");
	spec = getenv("rootspec");
	if (spec == nil)
		spec = strdup("main/active");
	seprint(cfg, cfg+128, "tcp!%s!564	%s	/	/	'sys=fs'", fs, spec);
	cmdline(cfg, "std", 1);
	uspec = getenv("usrspec");
	if (uspec != nil)
		seprint(cfg, cfg+128, "tcp!%s!564	%s	/usr	/usr	'sys=fs'", fs, uspec);
	else
		seprint(cfg, cfg+128, "tcp!%s!564	%s	/usr	/usr	'sys=fs'", fs, spec);
	cmdline(cfg, "std", 2);
	if (access("#s/vfossil", AEXIST) == 0){
		seprint(cfg, cfg+128, "/srv/vfossil	main/active	/	/	'sys=%s'", getenv("sysname"));
		cmdline(cfg, "std", 3);
		seprint(cfg, cfg+128, "/srv/vfossil	main/active	/usr	/usr	'sys=%s'", getenv("sysname"));
		cmdline(cfg, "std", 4);
	}
}

void
config(char* fname)
{
	int	fd;

	if (fname == nil)
		stdconfig();
	else {
		fd = open(fname, OREAD);
		if (fd < 0){
			fprint(2, "%s: %s: %r\n", argv0, fname);
			return;
		}
		fdconfig(fd, fname);
	}
}
