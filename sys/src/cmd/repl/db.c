#include <u.h>
#include <libc.h>
#include <bio.h>
#include <error.h>
#include <b.h>
#include <avl.h>
#include "repl.h"

int
dbcmp(Avl* l, Avl* r)
{
	Dbent*	ldb = (Dbent*)l;
	Dbent*	rdb = (Dbent*)r;

	return strcmp(ldb->fname, rdb->fname);
}

Dbent*
readdbent(Biobufhdr* bp)
{
	char*	ln;
	Dbent*	dp;
	char*	p;
	char*	e;

	while (ln = Brdstr(bp, '\n', 1)){
		dp = emalloc(sizeof(Dbent));
		memset(dp, 0, sizeof(Dbent));

		dp->fname = ln;
		p = strchr(ln, ' ');
		if (p == nil)
			goto bad;
		*p++ = 0;

		dp->hist = p;
		p = strchr(p, ' ');
		if (p == nil)
			goto bad;
		*p++ = 0;

		dp->uid = p;
		p = strchr(p, ' ');
		if (p == nil)
			goto bad;
		*p++ = 0;

		dp->gid = p;
		p = strchr(p, ' ');
		if (p == nil)
			goto bad;
		*p++ = 0;

		dp->mode  = strtoul(p, &e, 16);
		p = e++;

		dp->mtime = strtoul(p, &e, 16);
		p = e++;

		dp->length = strtoll(p,&e, 10);
		p = e;
		if (p && *p == ' ')
			dp->vers = strtoul(p+1, nil, 10);
		else
			dp->vers = ~0;
		dp->fname= estrdup(dp->fname);
		dp->hist = estrdup(dp->hist);
		dp->hlen = strlen(dp->hist);
		dp->uid  = estrdup(dp->uid);
		dp->gid  = estrdup(dp->gid);
		free(ln);
		return dp;
	bad:
		warn("malformed db entry for %s", dp->fname);
		free(ln);
		free(dp);
	}
	return nil;
}

void
insertdb(Db* db, Dbent* dp)
{
	Dbent*	old;

	insertavl(db->tree, (Avl*)dp, (Avl**)&old);
	if (old)
		free(old);
}

Db*
readdb(Biobufhdr* bp)
{
	Db*	db;
	Dbent*	dp;
	char*	ln;

	ln = Brdstr(bp, '\n', 1);
	if (ln == nil)
		return nil;
	db = emalloc(sizeof(Db));
	memset(db, 0, sizeof(Db));
	db->id = ln[0];
	if (strstr(ln, "replica")) // old format
		db->mtime=0UL;
	else
		db->mtime = strtoul(ln+2, nil, 16);
	free(ln);
	db->tree = mkavltree(dbcmp);
	while(dp = readdbent(bp))
		insertdb(db, dp);
	return db;
}

Db*
newdb(char id)
{
	Db*	db;

	db = emalloc(sizeof(Db));
	memset(db, 0, sizeof(Db));
	db->id = id;
	db->tree = mkavltree(dbcmp);
	db->mtime = time(nil);
	return db;
}

Db*
readdbfile(char* file)
{
	Db*	db;
	Biobuf* bin;
	Dir*	d;

	d = dirstat(file);
	if (d != nil){
		if (d->qid.type&QTDIR)
			error("dbfile is a directory.");
		free(d);
	}
	bin = Bopen(file, OREAD);
	if (bin == nil)
		error("can't open %s: %r", file);
	db = readdb(bin);

	/* We'll keep the db locked until the caller process
	 * 
	 * Bterm(bin);
	 */
	return db;
}


void
writedb(Biobufhdr* bp, Db* db)
{
	Avlwalk*w;
	Dbent*	dp;

	Bprint(bp, "%c %08ulx repl db:\n", db->id, db->mtime);
	w = avlwalk(db->tree);
	while(dp = (Dbent*)avlnext(w)){
		if (Bprint(bp, "%s %s %s %s %08ulx %08ulx %lld %uld\n",
			dp->fname, dp->hist,
			dp->uid, dp->gid, dp->mode, dp->mtime, dp->length,
			dp->vers) < 0)
			sysfatal("writedb");
	}
	endwalk(w);
}


void
writedbfile(char* file, Db* db)
{
	Biobuf* bout;
	Dir*	dp;
	Dir	d;

	bout = Bopen(file,OWRITE|OTRUNC);
	if (bout == nil)
		error("can't open %s: %r", file);
	writedb(bout, db);
	if (Bterm(bout) < 0)
		error("closing %s: %r", file);

	dp = dirstat(file);
	if (dp != nil){
		if (!(dp->mode&DMEXCL)){
			nulldir(&d);
			d.mode = dp->mode|DMEXCL;
			dirwstat(file, &d);
		}
		free(dp);
	}
}


void
rename(char* to, char* frompath)
{
	Dir	d;
	char*	p;

	remove(to);
	p = strrchr(to, '/');
	if (p != nil)
		to = p + 1;
	nulldir(&d);
	d.name = to;
	if (dirwstat(frompath, &d) < 0)
		error("can't rename %s: %r", frompath);
}
