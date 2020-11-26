#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <error.h>
#include <b.h>
#include <avl.h>
#include "repl.h"


#define vprint	if(verbose)print

int	verbose;
char**	skip;
int	nskip;
int	hpruneid;
int	updated;
int	notreeprunes;

Dbent*
newent(char* dir, Dir* d)
{
	Dbent*	np;

	np = emalloc(sizeof(Dbent));
	memset(np, 0, sizeof(Dbent));
	np->fname = smprint("%s/%s", dir, d->name);
	np->mode = d->mode;
	np->mtime= d->mtime;
	np->length = d->length;
	np->vers = d->qid.vers;
	np->uid = estrdup(d->uid);
	np->gid = estrdup(d->gid);

	return np;
}

static Dbent*	dirpruned[4096*2];
static int	ndirpruned = 0;

static void
prunedir(Dbent* p)
{

	if (0)
		vprint("Pruned %s\n", p->fname);
	p->pruned = 1;
	assert(ndirpruned < nelem(dirpruned));
	dirpruned[ndirpruned++] = p;
}

static int
contained(Dbent* a, Dbent* top)
{
	int	l;

	l = strlen(top->fname);
	if (strncmp(a->fname, top->fname, l) == 0)
	if (a->fname[l] == '/' || a->fname[l] == 0)
		return 1;
	return 0;
}

static int
pruneddir(Dbent* p)
{
	int	i;

	if (p->pruned)
		return 1;
	for (i = 0; i < ndirpruned; i++)
		if (contained(p, dirpruned[i]))
			return 1;
	return 0;
}

void
scan(Db* db, char* dir, ulong mtime)
{
	static	int	depth = 0;
	int	fd;
	int	i;
	int	n;
	Dir*	d;
	Dbent*	np;
	char*	fn;

	if (depth++ > 90 && !(depth % 100))
		fprint(2, "scandb: file tree dept >%d (bad db?)\n", depth);
	if (nskip){
		for (i = 0; i <nskip; i++){
			if (!strcmp(skip[i], dir)){
				depth--;
				return;
			}
		}
	}
	if (dir[0])
		fn = smprint("%s/%s", db->dir, dir);
	else
		fn = estrdup(db->dir);
	if (0)
		fprint(2, "scan: %s\n", fn);
	fd = open(fn, OREAD);
	if (fd < 0){
		depth = 0;
		error("can't read %s: %r", fn);
	}
	n = dirreadall(fd, &d);
	close(fd);
	for (i = 0; i < n; i++){
		np = newent(dir, &d[i]);
		insertdb(db, np);
		if (d[i].mode&DMDIR){
			if (d[i].mtime < mtime && !notreeprunes)
				prunedir(np);
			else
				scan(db, np->fname, mtime);
		}
	}
	free(d);
	depth--;
}

/* keep in sync with syncdb.c:/^cmpfile
 */
int
cmpfile(Meta* op, Meta* np)
{
	if (op->mode != np->mode ||
	    strcmp(op->uid, np->uid) || strcmp(op->gid, np->gid))
		return Metachg;
	if ((op->mode&DMDIR)==0){
		if (op->mode != np->mode)
			return Metachg;
		if (op->vers != np->vers && op->vers != ~0 && np->vers != ~0)
			return Datachg;
		if (op->length != np->length || op->mtime != np->mtime)
			return Datachg;
	}
	return Eq;
}



void
updatedb(Db* old, Db* new)
{
	Avlwalk*w;
	Dbent*	op;
	Dbent*	np;
	Dbent*	pp;
	char*	o;
	int	changed;

	w = avlwalk(old->tree);
	while(op = (Dbent*)avlnext(w)){
	again:
		if (pruneddir(op)){
			/*vprint("pruned %s\n", op->fname);*/
			pp = op;
			while(op = (Dbent*)avlnext(w)){
				if (!contained(op, pp))
					goto again;
			}
			if (op == nil)
				break;
		}
		np = (Dbent*) lookupavl(new->tree, (Avl*)op);
		if (np == nil){
			if(!isupper(op->hist[op->hlen-1])){
				vprint("d %s\n", op->fname);
				o = op->hist;
				op->hist = smprint("%s%c", o, toupper(old->id));
				op->mtime = time(nil);
				free(o);
			}
			continue;
		}
		np->visited = 1;
		changed = cmpfile(op, np);
		if (changed && !updated){
			if (changed == Metachg){
				vprint("m %s\n", op->fname);
			} else {
				vprint("c %s\n", op->fname);
			}
			o = op->hist;
			op->hist = smprint("%s%c", o, old->id);
			op->Meta = np->Meta;
			op->uid = estrdup(op->uid);
			op->gid = estrdup(op->gid);
			free(o);
		} else {
			// update entries from old dbs
			// that did not have qid.vers
			op->vers = np->vers;
		}
	}
	endwalk(w);

	/* 2. Check for unvisited nodes in new
	 *    that would correspond to new files.
	 */
	w = avlwalk(new->tree);
	while(np = (Dbent*)avlnext(w)){
		if (!np->visited && !np->pruned){
			vprint("a %s\n", np->fname);
			op =  emalloc(sizeof(Dbent));
			memset(op, 0, sizeof(Dbent));
			op->fname = estrdup(np->fname);
			op->hist = smprint("%c", old->id);
			op->Meta = np->Meta;
			op->uid = estrdup(op->uid);
			op->gid = estrdup(op->gid);
			insertdb(old, op);
		}
	}
	endwalk(w);
}

static void
prunedb(Db* db, int id)
{
	Avlwalk*w;
	Dbent*	dp;
	char	last;
	w = avlwalk(db->tree);
	while(dp = (Dbent*)avlnext(w)){
		last = dp->hist[dp->hlen-1];
		dp->hist = smprint("p%d%c", id, last);
		dp->hlen = 1;
	}
	endwalk(w);

}

void
usage(void)
{
	fprint(2, "usage: %s [-vut] [-p n] [-n id] [-r replid] [-i excldir] dir db\n", argv0);
	exits("usage");
}


void
main(int argc, char **argv)
{
	Db*	db;
	Db*	ndb;
	int	i;
	char	id;
	char	replid;
	Error	e;
	char*	nfname;
	char*	ofname;
	ulong	t;
	int	nopt;

	id = 0;
	replid = 0;
	nopt = 0;
	ARGBEGIN {
	case 'n':
		nopt = 1;
		id = *EARGF(usage());
		break;
	case 't':
		notreeprunes = 1;
		break;
	case 'p':
		hpruneid = *EARGF(usage());
		break;
	case 'r':
		replid = *EARGF(usage());
		break;
	case 'v':
		verbose = 1;
		break;
	case 'u':
		updated = 1;
		break;
	case 'i':
		if ((nskip%Incr) == 0)
			skip = realloc(skip, sizeof(char*)*(nskip+Incr));
		skip[nskip++] = EARGF(usage());
		break;
	default:
		usage();
	} ARGEND;
	if (argc != 2)
		usage();
	errinit(&e);
	if (catcherror()){
		sysfatal("%r");
	}

	if (catcherror()){
		if (!id)
			usage();
		db = newdb(id);
	} else {
		db = readdbfile(argv[1]);
		noerror();
	}
	if (*argv[0] != '/')
		sysfatal("replica dirs must be absolute");
	for (i = 0; i < nskip; i++){
		if (*skip[i] != '/')
			skip[i] = smprint("/%s", skip[i]);
	}

	nfname = smprint("%s.new", argv[1]);
	ofname = smprint("%s.old", argv[1]);
	if (hpruneid){
		prunedb(db, hpruneid);
		writedbfile(nfname, db);
	} else {
		db->dir = cleanname(estrdup(argv[0]));
		ndb = newdb(db->id);
		ndb->dir = estrdup(db->dir);
		t = time(nil);
		scan(ndb, "", db->mtime);
		if (replid)
			db->id = replid;
		if (!notreeprunes)
			db->mtime = t;
		updatedb(db, ndb);
		writedbfile(nfname, db);
	}
	if (!nopt)
		rename(ofname, argv[1]);
	rename(argv[1], nfname);
	noerror();
	exits(nil);
}
