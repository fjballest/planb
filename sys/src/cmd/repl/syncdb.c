#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <error.h>
#include <b.h>
#include <avl.h>
#include "repl.h"

#define vprint	if(verbose)print
#define dprint	if(debug)print

int pri;
int verbose;
int dryrun;
int dont;
int debug;
int force;

char**	dirdels;
int	ndirdels;

char**	syncdirs;
int	nsyncdirs;


static void
adddirdel(char* d)
{
	if ((ndirdels%Incr) == 0)
		dirdels = erealloc(dirdels, (ndirdels+Incr)*sizeof(char*));
	dirdels[ndirdels++] = estrdup(d);
}

static int
contained(char* fname, char* top)
{
	int	l;

	l = strlen(top);
	if (strncmp(fname, top, l) == 0)
	if (fname[l] == '/' || fname[l] == 0)
		return 1;
	return 0;
}

static void
addsyncdir(char* d)
{
	if (d[0] == '/' || (d[0] == '.' && (d[1] == '/' || d[1] == 0))){
		fprint(2, "%s: not relative from repl root.\n", d);
		exits("baddir");
	}

	if ((nsyncdirs%Incr) == 0)
		syncdirs = erealloc(syncdirs, (nsyncdirs+Incr)*sizeof(char*));
	syncdirs[nsyncdirs++] = smprint("/%s", d);
}


static int
mustsync(char* d)
{
	int	i;

	if (nsyncdirs == 0)	// no dirs: sync it all.
		return 1;

	for (i = 0; i < nsyncdirs; i++)
		if (contained(d, syncdirs[i]))
			return 1;
	return 0;
}

static void
deldirs(void)
{
	int	i;

	for (i = ndirdels - 1; i >= 0; i--){
		dprint("remove %s\n", dirdels[i]);
		remove(dirdels[i]);
	}
}

/* Keep in sync with scandb.c:/^cmpfile
 * But be very careful not to compare qid.vers here,
 * because qids come from different servers.
 */
static int
cmpfile(Dir* op, Dir* np)
{
	if ((op->qid.type&QTDIR)==0){
		if (op->length != np->length || op->mtime != np->mtime)
			return Datachg;
		if (op->mode != np->mode)
			return Metachg;
	}
	if (op->mode != np->mode ||
	    strcmp(op->uid, np->uid) || strcmp(op->gid, np->gid))
		return Metachg;
	return Eq;
}

static int
deletefile(char* to, char* fname, ulong dmtime)
{
	char* dfile;
	char* e;
	Dir*  dd;

	dfile = smprint("%s%s", to, fname);
	dd = dirstat(dfile);
	if (dd != nil){
		if (dd->mode&DMDIR){
			adddirdel(dfile);
			return 1;
		}
		if (!force && dmtime && dmtime != dd->mtime){
			free(dd);
			error("locally modified: %s: won't delete", dfile);
		}
		free(dd);
	}
	dprint("remove %s\n", fname);
	if (remove(dfile)<0){
		e = smprint("remove %s: %r\n", dfile);
		if (!strstr(e, "does not exist")){
			warn(e);
			free(e);
			return 0;
		}
		free(e);
	}
	return 1;
}

static void
copytofd(int fd, char* from)
{
	static char buf[64*1024];
	int	ffd;
	int	n;

	ffd = open(from, OREAD);
	if (ffd < 0)
		error("open %s: %r", from);
	while(n = read(ffd, buf, sizeof(buf))){
		if (n < 0)
			error("read %s: %r", from);
		if (write(fd, buf, n) != n)
			error("copying %s: %r", from);
	}
	close(ffd);
}

/*
 * This synchronizes the files according to their real FS.
 * The metadata within the DB is ignored.
 * The worst that can happen is that a more recent file is
 * copied, but according to the DB the file has changed anyway.
 * Perhaps relying just on the DB to see what to do would simplify
 * this looong routine.
 */

static int
syncfile(char* to, char* from, char* fname, ulong dmtime, ulong* vers)
{
	char*	dfile;
	char*	tfile;
	char*	sfile;
	Dir*	sd;
	Dir*	dd;
	Dir*	td;
	Dir	wd;
	int	fd;
	int	what;
	int	omode;
	char*	newfile;
	int	isnew;

	dfile = smprint("%s%s", to, fname);
	tfile = smprint("%s%s!tmp", to, fname);
	sfile = smprint("%s%s", from, fname);
	dprint("syncfile: %s: %s -> %s \n", fname, from, to);
	newfile = nil;
	sd = dirstat(sfile);
	dd = nil;
	fd = -1;
	if (catcherror()){
		free(dfile);
		free(tfile);
		free(sfile);
		free(sd);
		free(dd);
		if (newfile)
			remove(newfile);
		if (fd != -1)
			close(fd);
		fprint(2, "%r\n");
		return 0;
	}

	if (sd == nil)
		error("can't stat %s: %r", sfile);

	dd = dirstat(dfile);
	if (dd == nil)
		what = Datachg;
	else {
		if (!force && !(dd->mode&DMDIR) && dmtime && dd->mtime != dmtime)
			error("locally modified: %s: wont update", dfile);
		what = cmpfile(dd, sd);
		if (what == Datachg && (dd->mode&DMAPPEND))
			error("append-only file: %s: wont update", dfile);
	}
	if (sd->qid.type&QTDIR)
		omode = OREAD;
	else
		omode = OWRITE|OTRUNC;
	nulldir(&wd);
	isnew = 0;
	switch(what){
	case Eq:
		warn("same file: %s", dfile);
		td = dirstat(dfile);
		if (td != nil){
			*vers = td->qid.vers;
			free(td);
		}
		break;
	case Datachg:
		if (dd == nil){
			fd = create(tfile, omode, 0755|(sd->mode&DMDIR));
			if (fd < 0)
				error("can't create %s: %r", tfile);
			newfile = tfile;
			isnew = 1;
		} else {
			fd = open(dfile, omode);
			if (fd < 0)
				error("open %s: %r", dfile);
		}
		if (!(sd->qid.type&QTDIR)){
			copytofd(fd, sfile);
			if (!isnew)
				dprint("c %s\n", fname);
		}
		td = dirfstat(fd);
		if (td != nil){
			*vers = td->qid.vers;
			free(td);
		}
		if (close(fd)<0)
			error("close %s: %r", dfile);
		if (isnew){
			wd.name = sd->name;
			if (dirwstat(tfile, &wd) < 0)
				error("can't rename %s: %r", tfile);
			nulldir(&wd);
			newfile = nil;
			dprint("a %s\n", fname);
		}
		// fall through...
	case Metachg:
		if (dd == nil || dd->mode != sd->mode){
			wd.mode = sd->mode;
			if (dirwstat(dfile, &wd) < 0)
				error("can't wstat %s: %r", dfile);
		}
		if (!(sd->qid.type&QTDIR))
		if (dd == nil || dd->mtime != sd->mtime)
			wd.mtime = sd->mtime;
		if (dd == nil || strcmp(dd->uid, sd->uid))
			wd.uid = sd->uid;
		if (dd == nil || strcmp(dd->gid, sd->gid))
			wd.gid = sd->gid;
		if (dirwstat(dfile, &wd) < 0)
			warn("can't set mtime/uids: %s: %r", dfile);
		if (!isnew)
			dprint("m %s\n", fname);
		break;
	}

	noerror();
	free(dfile);
	free(tfile);
	free(sfile);
	free(sd);
	free(dd);
	if (newfile)
		remove(newfile);
	return 1;
}

static void
syncdbent(Db* to, Dbent* tdp, Dbent* fdp, ulong vers)
{

	to->changed = 1;
	if (tdp == nil){
		tdp = emalloc(sizeof(Dbent));
		memset(tdp, 0, sizeof(Dbent));
		tdp->fname = estrdup(fdp->fname);
		tdp->next = to->new;
		to->new = tdp;
	}
	tdp->hist = estrdup(fdp->hist);
	tdp->hlen = fdp->hlen;
	tdp->Meta = fdp->Meta;
	tdp->vers = vers;
}

static void
addnewents(Db* db)
{
	Dbent*	dp;

	while(db->new != nil){
		dp = db->new;
		db->new = dp->next;
		// dp->next = nil; not needed.
		insertdb(db, dp);
	}
}

static void
change(Db* to, Db* from, Dbent* tdp, Dbent* fdp)
{
	int	isdel;
	ulong	dmtime;
	ulong	vers;

	assert(fdp != nil);
	if (dont == to->id)
		return;
	if (0)
		vprint("%c %s %s\n", from->id, fdp->fname, fdp->hist);

	isdel = isupper(fdp->hist[fdp->hlen-1]);
	dmtime = tdp ? tdp->mtime : 0L;
	if (isdel){
		if (tdp){
			vprint("d %s%s\n", to->dir, tdp->fname);
			if (!dryrun && deletefile(to->dir, tdp->fname, dmtime))
				syncdbent(to, tdp, fdp, 0);
		} else
			syncdbent(to, nil, fdp, 0);
	} else {
		vprint("c %s%s\n", to->dir, fdp->fname);
		if (!dryrun)
		if (syncfile(to->dir, from->dir, fdp->fname, dmtime, &vers))
			syncdbent(to, tdp, fdp, vers);
	}
	// Directories deleted in reverse order: they must be empty.
	deldirs();
}

static void
conflict(char* fname, char* h1, char* h2)
{
	int	i;

	print("! %s:  after [", fname);
	for (i = 0; h1[i] && h2[i] && h1[i] == h2[i]; i++)
		print("%c", h1[i]);
	print("] either [%s] or [%s].\n", h1+i, h2+i);
}

static void
changes(Db* db1, Db* db2)
{
	Avlwalk*w;
	Dbent*	dp1;
	Dbent*	dp2;
	int	l;

	w = avlwalk(db1->tree);
	while(dp1 = (Dbent*)avlnext(w)){
		if (!mustsync(dp1->fname))
			continue;
		dp2 = (Dbent*)lookupavl(db2->tree, (Avl*)dp1);
		if (dp2 == nil){
			change(db2, db1, nil, dp1);
			continue;
		}
		dp2->visited = 1;
		l = dp1->hlen;
		if (dp2->hlen < l)
			l = dp2->hlen;
		if (strncmp(dp1->hist, dp2->hist, l) != 0){
			if (pri == db1->id)
				change(db2, db1, dp2, dp1);
			else if (pri == db2->id)
				change(db1, db2, dp1, dp2);
			else
				conflict(dp1->fname, dp1->hist, dp2->hist);
		} else if (dp1->hlen < dp2->hlen){
			change(db1, db2, dp1, dp2);
		} else if (dp1->hlen > dp2->hlen){
			change(db2, db1, dp2, dp1);
		}
	}
	endwalk(w);

	w = avlwalk(db2->tree);
	while(dp2 = (Dbent*)avlnext(w)){
		if (!mustsync(dp2->fname))
			continue;
		if (!dp2->visited)
			change(db1, db2, nil, dp2);
	}
	endwalk(w);

	// Done with avlwalks. Add any new node.
	addnewents(db1);
	addnewents(db2);
}

static void
usage(void)
{
	fprint(2, "usage: %s [-12Dflrnv] dir1 dir2 db1 db2 [syncdir...]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	Db*	db1;
	Db*	db2;
	char*	nfname;
	int	i;
	Error	e;

	ARGBEGIN {
	case '1':
		pri=1;
		break;
	case '2':
		pri=2;
		break;
	case 'D':
		debug=1;
		break;
	case 'f':
		force=1;
		break;
	case 'l':
		dont=2;
		break;
	case 'r':
		dont=1;
		break;
	case 'v':
		verbose=1;
		break;
	case 'n':
		dryrun=verbose=1;
		break;
	default:
		usage();
	} ARGEND;

	if (argc < 4)
		usage();
	errinit(&e);
	if (catcherror()){
		sysfatal("%r");
	}

	for (i = 4; i < argc; i++)
		addsyncdir(argv[i]);

	db1 = readdbfile(argv[2]);
	db2 = readdbfile(argv[3]);
	if (*argv[0] != '/' || *argv[1] != '/')
		sysfatal("replica dirs must be absolute");
	db1->dir = cleanname(estrdup(argv[0]));
	db2->dir = cleanname(estrdup(argv[1]));
	if (!strcmp(db1->dir, db2->dir) || db1->id == db2->id)
		sysfatal("same replica");

	if (pri == 1)
		pri = db1->id;
	else if (pri == 2)
		pri = db2->id;
	if (dont == 1)
		dont = db1->id;
	else if (dont == 2)
		dont = db2->id;

	changes(db1, db2);

	if (db1->changed){
		nfname = smprint("%s.new", argv[2]);
		writedbfile(nfname, db1);
		rename(argv[2], nfname);
	}
	if (db2->changed){
		nfname = smprint("%s.new", argv[3]);
		writedbfile(nfname, db2);
		rename(argv[3], nfname);
	}
	noerror();
	exits(nil);
}
