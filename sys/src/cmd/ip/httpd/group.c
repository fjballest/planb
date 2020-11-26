/*
 * Web discussion groups.
 * The group is identified by an argument.
 * Each group is kept as a flat directory.
 * There we keep a file per article.
 * Each file is made of a series of sections.
 * The first section is considered the article
 * The following ones are the replies.
 * The Id determines to which post they are replying.
 * Number | Id.Number
 * title
 * Author
 * body lines...
 * \a (ascii 07 / bell)
 */


#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include "httpd.h"
#include "httpsrv.h"
#include "article.h"

enum {
	Nfront	= 10,	// max # in front page
};


typedef struct Req  Req;
struct Req{
	char*	group;
	char*	file;
	int	op;
	HSPairs*args;
};

typedef struct Opf Opf;
struct Opf {
	char	op;
	void	(*f)(Req*);
};

static	Hio*	hout;
static	Hio	houtb;
static	int	vermaj;
static	HConnect*hcon;

static	int	debug = 0;


static	char	Dfltreq[]= "o=i&g=dso";
static	char	Top[]	= "/groups";
static	char	Mark[]	= "DYNAMIC";

/* 
 * Strings seen in output, to help with localization.
 */
static	char	Msearchresults[] = "Resultados de la búsqueda";
static	char	Mreturn[] = "Volver.";
static	char	Msendnew[]= "Enviar nuevo articulo";
static	char	Mreply[] = "Responder";
static	char	Mreplyto[]= "Responder a...";
static	char	Manswers[] = "resp.";
static	char	Mgoonreading[] = "Seguir leyendo otros";
static	char	Mgoback[] = "Volver atras";
static	char	Mreadmore[] = "Leer mas";
static	char	Mreturntoindex[] = "Volver a la portada";
static	char	Mwrote[] = "dice...";
static	char	Mby[] = "Por";
static	char	Mrequiredfieldmissing[] =
		"No has escrito en un campo requerido.\n"
		"Usa back en tu navegador e intenta de nuevo\n";
static	char	Mclickheretoreturn[] = "<p>Hecho. Pulsa "
		"<a href=\"/magic/group?o=i&g=%s\">aquí</a> "
		"para volver.\n";
static	char	Mbodywithlonglines[]= 
	"Tienes lineas muy largas. Usa back en tu navegador\n"
	"y usa return para que tu mensaje tenga lineas\n"
	"que se puedan leer bien (max 80 caracteres).\n";

static void
badreq(void)
{
	hfail(hcon, HBadReq);
	exits(nil);
}

static void
notfound(char* o)
{
	hfail(hcon, HNotFound, o);
	exits(nil);
}

static void
failure(void)
{
	hfail(hcon, HTempFail);
	exits(nil);
}

static void
headers(char* title)
{
	if(vermaj){
		hokheaders(hcon);
		hprint(hout, "Content-type: text/html; charset=UTF-8\r\n");
		hflush(hout);
	}
	if (title != nil)
		hprint(hout, "<head><title>%s</title></head><body>\n", title);
}

static void
notice(char* fmt, ...)
{
	va_list arg;
	char buf[1024], *out;

	va_start(arg, fmt);
	out = vseprint(buf, buf+sizeof(buf), fmt, arg);
	va_end(arg);
	*out = 0;
	headers("notice");
	hprint(hout, "%s", buf);
	hprint(hout, "</body>\n");
}

static char*
getarg(Req* r, char arg, int must)
{
	HSPairs*p;

	for (p = r->args; p != nil; p = p->next){
		if (p->s[0] == arg)
			return p->t;
	}
	if (must)
		badreq();
	return nil;
}

static void
parsereq(char* s, Req* r)
{
	char*	v;

	memset(r, 0, sizeof(Req));
	r->args = hparsequery(hcon, s);

	v = getarg(r, 'o', 1);
	r->op = *v;
	v = getarg(r, 'g', 1);
	r->group = strdup(v);
}

static void
outfilehdr(Biobuf* bin)
{
	char*	ln;

	while(ln = Brdstr(bin, '\n', 1)){
		if (strcmp(ln, Mark) == 0){
			free(ln);
			return;
		}
		hprint(hout, "%s\n", ln);
		free(ln);
	}
}

static void
outfiletail(Biobuf* bin)
{
	char*	ln;

	while(ln = Brdstr(bin, '\n', 1)){
		hprint(hout, "%s\n", ln);
		free(ln);
	}
}

static void
getpostintro(Post* p, char* intro, int max, int* more)
{
	char*	e;
	char*	w;
	int	l;

	e = strecpy(intro, intro+max-4, p->body);
	l = e - intro;
	*more = (p->body[l] != 0);
	if (*more){
		w = strrchr(intro, ' ');
		if (w)
			e = w;
		strcpy(e, "...");
	}
}

static char* Mon[] = {
	"Ene", "Feb", "Mar", "Abr", "May", "Jun",
	"Jul", "Ago", "Sep", "Oct", "Nov", "Dic"
};

static char*
artdate(ulong mt)
{
	Tm*	tm;
	static char str[40];

	if (mt == 0)
		return "";
	tm = localtime(mt);
	seprint(str, str+40, "(%d-%s)", tm->mday, Mon[tm->mon]);
	return str;
}

static Biobuf*
bopenfile(char* group, char* art, ulong* mt)
{
	char*	file;
	Biobuf*	ib;
	Dir*	d;

	file = smprint("%s/%s/%s", Top, group, art);
	if (strstr(file, "..")){
		free(file);
		badreq();
	}
	if (mt != nil){
		*mt = 0;
		d = dirstat(file);
		if (d != nil){
			*mt = d->mtime;
			free(d);
		}
	}
	ib = Bopen(file, OREAD);
	free(file);
	return ib;
}

static int
outpostintro(char* g, char* file, int verbose)
{
	Biobuf*	bin;
	Post*	p;
	int	n;
	char	intro[120];
	int	more;
	char*	date;
	ulong	mt;

	bin = bopenfile(g, file, &mt);
	if (bin == nil)
		return 0;
	if (debug)
		fprint(2, "%s/%s\n", g, file);
	p = readarticle(bin, &n);
	if (p == nil){
		Bterm(bin);
		return 0;
	}
	date = artdate(mt);
	if(debug)
		fprint(2, "article %s\n", p->title);
	switch(verbose){
	case 0:
		hprint(hout,"<b><a href=\"/magic/group?o=a&g=%s&a=%s\">",
			g, file);
		hprint(hout, "%s</a> (%s; %d posts)</b><br>\n", p->title, p->author, n);
		break;
	case 1:
		hprint(hout,"<p> <a href=\"/magic/group?o=a&g=%s&a=%s\">",
			g, file);
		hprint(hout, "<font size=+1><b>%s</b> %s</a> ", p->title, date);
		hprint(hout, "<font size=-1><b>(%s %s)</b><br>\n", Mby, p->author);
		break;
	default:
		hprint(hout, "<p><font size=+1><hr><b>%s</b> %s", p->title, date);
		hprint(hout, "<br><font size=-1><b>%s %s</b> <br>\n", Mby, p->author);
		getpostintro(p, intro, sizeof(intro), &more);
		hprint(hout, "<font size=+0>%s<br>\n", intro);
		if (n > 1)
			hprint(hout,
				"<a href=\"/magic/group?o=a&g=%s&a=%s\">Leer "
				"mas</a> (%d %s.)<p>\n", g, file, n-1, Manswers);
		else
			hprint(hout,
				"<a href=\"/magic/group?o=a&g=%s&a=%s\">%s"
				"</a><p>\n",g, file, Mreadmore);
	}
	closepost(p);
	Bterm(bin);
	return 1;
}

static char*
readfile(char* fname)
{
	Dir*	d;
	vlong	len;
	int	fd;
	char*	buf;

	fd = open(fname, OREAD);
	if (fd < 0)
		return nil;
	d = dirfstat(fd);
	assert (d != nil);
	len = d->length;
	free(d);
	buf = malloc(len +1);
	buf[0] = 0;
	readn(fd, buf, len);
	close(fd);
	buf[len] = 0;
	return buf;
}

static int
idxsort(void* a, void* b)
{
	Dir* da = a;
	Dir* db = b;

	if (da->name[0] != 'A')
		return 1;
	return (int)(da->mtime - db->mtime);
}

static char*
readindex(Req* req, char*** entsp, int* nentsp)
{
	Dir*	d;
	char**	ents;
	int	fd;
	char*	buf;
	char*	dir;
	int	nd;
	long	l;
	int	i, j;
	char*	s;

	/* Experiment, read all article files, and
	 * sort by mtime.
	 */
	if (strstr(req->group, ".."))
		notfound("..");
	dir = smprint("%s/%s", Top, req->group);
	fd = open(dir, OREAD);
	if (fd < 0)
		failure();
	nd = dirreadall(fd, &d);
	close(fd);
	if (nd <= 0){
		*entsp = nil;
		*nentsp = 0;
		return nil;
	}
	qsort(d, nd, sizeof(d[0]), idxsort);
	l = 0;
	for (i = 0; i < nd; i++){
		if (d[i].name[0] != 'A')
			continue;
		l += strlen(d[i].name) + 2;
	}
	s = buf = malloc(l);
	ents=malloc(sizeof(char*)*(nd+1));
	if (buf == nil){
		free(d);
		failure();
	}
	for (i = j = 0; i < nd; i++){
		//hprint(hout, "debug: art = %s\n<br>", d[i].name);
		if (d[i].name[0] == 'A'){
			ents[j++] = s;
			s = strecpy(s, buf+l, d[i].name);
			*s++ = 0;
		}
	}
	*entsp = ents;
	*nentsp= j;
	free(d);
	return buf;
}

static void
getidxreq(Req* req)
{
	static char prevfmt[] = "<a href=\"/magic/group?o=i&g=%s&S=%d\">%s</a><p>\n";
	static char nextfmt[] = "<a href=\"/magic/group?o=i&g=%s&S=%d\">%s</a><p>\n";
	char*	idx;
	char**	ents;
	int	nents;
	char*	buf;
	Biobuf*	ib;
	int	i;
	int	start;
	int	n;
	char*	s;

	idx = smprint("%s/%s/index.html", Top, req->group);
	start = 0;
	s = getarg(req, 'S', 0);
	if (s != nil)
		start=atoi(s);
	ib = Bopen(idx, OREAD);
	if (ib == nil)
		notfound("index");
	headers(nil);
	outfilehdr(ib);
	buf = readindex(req, &ents, &nents);
	n = 0;
	for (i = nents-1; i >= 0; i--){
		if (start != 0 && n == start){
			hprint(hout, prevfmt , req->group,
				((start - Nfront) < 0 ? 0 : (start - Nfront)), Mgoback);
		}
		if (n >= start && n < start+Nfront)
			outpostintro(req->group, ents[i], 2);
		if (n == start+Nfront){
			hprint(hout, nextfmt, req->group,
				start + Nfront, Mgoonreading);
			break;
		}
		n++;
	}
	outfiletail(ib);
	Bterm(ib);
	free(idx);
	free(ents);
	free(buf);
}

static void
outpost(int tab, char* g, char* f, Post* p)
{
	char	tabs[Nids+1];
	char	*np;
	char	*sp;
	char*	s;
	char*	titlefmt= "%s<font size=+2><b>%s</b><font size=-2>\n<p>";
	char*	authfmt= "%s<font size=+1><b>%s %s</b><font size=-1><br>\n";
	char*	otherfmt = "%s<font size=+1><b>%s %s</b><font size=-1><br>\n";
	char*	replfmt = "%s<a href=\"/magic/group?o=r&g=%s&a=%s&"
				"i=%s\">%s</a>\n";

	memset(tabs, '\t', sizeof(tabs));
	tabs[tab] = 0;

	if (tab <= 0){
		if (p->title[0] != 0)
			hprint(hout, titlefmt, tabs, p->title);
		hprint(hout, authfmt, tabs, Mby, p->author);
	} else
		hprint(hout, otherfmt, tabs, p->author, Mwrote);
	sp = p->body;
	if (p->body[0] != 0){
		while(sp != nil){
			np = strchr(sp, '\n');
			hprint(hout, "%s", tabs);
			if (np == nil)
				hprint(hout, "%s\n", sp);
			else{
				hwrite(hout, sp, np-sp);
				hprint(hout, "\n");
				np++;
			}
			sp = np;
		}
	}
	if (g != nil && f != nil){
		s = id2str(p->ids, p->nids);
		hprint(hout, replfmt, tabs, g, f, s, Mreply);
	}
	hprint(hout, "<hr>\n");
}

static void
outarticle(int tab, char* g, char* f, Post* p)
{
	int	i;

	outpost(tab, g, f, p);
	for (i = 0; i < p->nsons; i++)
		outarticle(tab+1, g, f, p->sons[i]);
}

static Post*
openarticle(char* group, char* art)
{
	Post*	p;
	Biobuf*	ib;
	ulong	mt;

	ib = bopenfile(group, art, &mt);
	p = readarticle(ib, nil);
	if (p == nil){
		Bterm(ib);
		notfound(art);
	}
	Bterm(ib);
	return p;
}

static void
getartreq(Req* req)
{
	Post*	p;
	Biobuf*	ib;

	ib = bopenfile(req->group, "article.html", nil);
	if (ib == nil)
		notfound("article.html");
	req->file = getarg(req, 'a', 1);
	// Could locate previous and following article in index
	// to output <next><prev> links near the bottom.
	p = openarticle(req->group, req->file);
	headers(nil);
	outfilehdr(ib);
	outarticle(0, req->group, req->file, p);
	hprint(hout, "<p><a href=\"/magic/group?o=i&g=%s\">%s</a>\n",
		req->group, Mreturntoindex);
	outfiletail(ib);
	Bterm(ib);
}

static void
outreplyform(Req* r, Post* p, char* form)
{
	Biobuf*	ib;
	char*	id;

	ib = bopenfile(r->group, form, nil);
	if (ib == nil)
		notfound(form);
	outfilehdr(ib);
	if (p == nil || r->file == nil){
		hprint(hout, "<h1>%s</h1>\n", Msendnew);
		hprint(hout,
			"<form action=\"/magic/group?o=P&g=%s\" "
			"method=\"post\">\n<p>",
		   	r->group);
	} else {
		hprint(hout, "<h1>%s</h1>\n<hr><pre>", Mreplyto);
		outpost(0, nil, nil, p);
		id = id2str(p->ids, p->nids);
		hprint(hout,
			"</pre><p><form action=\"/magic/group?o=P&g=%s&a=%s&i=%s\" "
			"method=\"post\">\n<p>",
		   	r->group, r->file, id);
		free(id);
	}
	outfiletail(ib);

	Bterm(ib);
}

static void
replyreq(Req* req)
{
	Post*	p;
	Post*	top;
	char*	id;
	int	ids[Nids];
	int	nids;
	Biobuf*	ib;
	char*	form;

	p = nil;
	req->file = getarg(req, 'a', 0);
	if (req->file == nil)
		form = "post.html";
	else {
		form = "reply.html";
		id = getarg(req, 'i', 0);
		if (id == nil)
			badreq();
		nids = parseid(id, ids, Nids);
		ib = bopenfile(req->group, req->file, nil);
		top = readarticle(ib, nil);
		if (top == nil)
			notfound(req->file);
		Bterm(ib);
		p = lookup(top, ids, nids);
		if (p == nil){
			closepost(top); // BUG: leak: sons
			badreq();
		}
	}
	headers(nil);
	outreplyform(req, p, form);
}

static void
putpost(Req* r, Post* np)
{
	static char buf[Maxpost];
	char*	e;
	Post*	p;
	int	fd;
	char*	fname;
	char	name[40];

	if (r->file != nil){
		p = openarticle(r->group, r->file);
		p = lookup(p, np->ids, np->nids);
		if (p == nil)
			notfound("previous article");
		closepost(p); // BUG: leaks sons
		if (np->nids == Nids)
			failure();
		np->ids[np->nids++] = truerand()%10000;
		fname = smprint("%s/%s/%s", Top, r->group, r->file);
		if (strstr(fname, ".."))
			badreq();
		fd = open(fname, OWRITE);
		free(fname);
	} else {
		fname = smprint("%s/%s/INDEX", Top, r->group);
		fd = open(fname, OWRITE);
		if (fd < 0)
			notfound("INDEX");
		free(fname);
		seprint(name, name+sizeof(name), "A%08uld",  truerand()%10000000);
		fname = smprint("%s/%s/%s", Top, r->group, name);
		if (strstr(fname, ".."))
			badreq();
		seek(fd, 0, 2);
		fprint(fd, "%s\n", name);
		close(fd);
		//fprint(2, "creating %s\n", fname);
		fd = create(fname, OWRITE, DMAPPEND|0666);
		free(fname);
		np->ids[0] = truerand()%10000;
		np->nids = 1;
	}
	if (fd < 0)
		notfound("post");
	seek(fd, 0, 2);
	e = seprintpost(buf, buf+sizeof(buf), np);
	write(fd, buf, e - buf);
	close(fd);
}

static int
checkbodyok(char* body)
{
	char*	ln;

	while(*body){
		ln = strchr(body+1, '\n');
		if (ln == nil)
			ln = body+strlen(body);
		if (ln - body > 80){
			notice(Mbodywithlonglines);
			return 0;
		}
		body = ln;
	}
	return 1;
}

static void
putreplyreq(Req* r)
{
	Post*	p;
	char*	id;

	p = newpost();
	id = getarg(r, 'i', 0);
	r->file  = getarg(r, 'a', 0);
	p->title = getarg(r, 'T', 0);
	p->author= getarg(r, 'A', 1);
	p->body  = getarg(r, 'B', 1);
	if(r->file == nil){	// new article
		if (!p->title)
			badreq();
		if (!p->title[0]){
			notice(Mrequiredfieldmissing);
			return;
		}
	}
	if (!p->author[0] || !p->body[0]){
		notice(Mrequiredfieldmissing);
		return;
	}
	if (!checkbodyok(p->body))
		return;
	if (id == nil){
		if (r->file != nil)
			badreq();
		p->nids = 0;
	} else{
		if (r->file == nil)
			badreq();
		p->nids = parseid(id, p->ids, Nids);
	}
	if (p->title == nil)
		p->title = strdup("");
	if (p->body == nil)
		p->body = strdup("");
	putpost(r, p);
	notice(Mclickheretoreturn, r->group);
}

static void
searchreq(Req* r)
{
	char*	words[10];
	int	nwords;
	char*	arg;
	char**	ents;
	int	nents;
	char*	buf;
	char*	fbuf;
	int	i, j;
	char*	fname;

	// All the Post has been parsed,
	// but for the extra from fields.

	arg = getarg(r, 'w', 1);
	nwords = tokenize(arg, words, nelem(words));
	buf = readindex(r, &ents, &nents);

	for (i = 0; i < nents; i++){
		fname = smprint("%s/%s/%s", Top, r->group, ents[i]);
		fbuf = readfile(fname);
		free(fname);
		if (fbuf != nil){
			for (j = 0; j < nwords; j++){
				if (cistrstr(fbuf, words[j]) == 0){
					ents[i] = nil;
					break;
				}
			}
			free(fbuf);
		} else
			ents[i] = nil;
	}
	headers("search");
	hprint(hout, "<body bgcolor=\"white\">\n");
	hprint(hout, "<h1>%s</h1>\n<hr><p>\n", Msearchresults);
	for (i = 0; i < nents; i++){
		if (ents[i] == nil)
			continue;
		outpostintro(r->group, ents[i], 2);
	}
	hprint(hout, "<p><hr><p><a href=\"/magic/group?o=i&g=%s\">%s"
		"</a></body>\n", r->group, Mreturn);
	free(buf);
	free(ents);
}

static Opf	ops[] = {
	{ 'i',	getidxreq },
	{ 'a',	getartreq },
	{ 'r',	replyreq },
	{ 'P',	putreplyreq },
	{ 's',	searchreq },
};

static void
request(char* req)
{
	Req	r;
	int	i;

	if (req == nil)
		req = Dfltreq;
	parsereq(req, &r);
	if (debug)
		fprint(2, "op %d, group %s\n", r.op, r.group);
	for (i = 0; i < nelem(ops); i++)
		if (r.op == ops[i].op){
			ops[i].f(&r);
			break;
		}
	if (i == nelem(ops))
		badreq();
}

void
main(int argc, char **argv)
{
	Hio*	hin;
	char*	t;
	char*	s;
	char*	opts;
	fmtinstall('H', httpfmt);
	fmtinstall('U', hurlfmt);

	if (debug)
		dup(open("/dev/cons", OWRITE), 2);
	else
		close(2);

	hcon = init(argc, argv);


	hout = &hcon->hout;
	if(hparseheaders(hcon, HSTIMEOUT) < 0)
		exits("failed");
	hcon->head.closeit = 1;
	if(strcmp(hcon->req.meth, "GET") != 0 && strcmp(hcon->req.meth, "HEAD") != 0)
	if(strcmp(hcon->req.meth, "POST") != 0){
		hunallowed(hcon, "GET, HEAD, PUT");
		exits("not allowed");
	}
	if(hcon->head.expectother){
		hfail(hcon, HExpectFail, nil);
		exits("failed");
	}
	if(hcon->head.expectcont){
		hprint(hout, "100 Continue\r\n");
		hprint(hout, "\r\n");
		hflush(hout);
	}
	if (strcmp(hcon->req.meth, "POST") == 0){
		s = nil;
		hin = hbodypush(&hcon->hin,
			hcon->head.contlen, hcon->head.transenc);
		if(hin != nil){
			alarm(15*60*1000);
			s = hreadbuf(hin, hin->pos);
			alarm(0);
		}
		if(s == nil){
			hfail(hcon, HBadReq, nil);
			exits("failed");
		}
		t = strchr(s, '\n');
		if(t != nil)
			*t = '\0';
		if (hcon->req.search != nil)
			opts = smprint("%s&%s", hcon->req.search, s);
		else
			opts = s;
	} else
		opts = hcon->req.search;
	vermaj = hcon->req.vermaj;

	truerand();	// open /dev/random before bind.
	bind("/usr/web", "/", MREPL);
	request(opts);
	hlflush(hout);
	hclose(hout);
	writelog(hcon, "200 group %ld %ld\n", hout->seek, hout->seek);
	exits(nil);
}
