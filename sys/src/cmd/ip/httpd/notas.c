#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include "httpd.h"
#include "httpsrv.h"

static	Hio		*hout;
static	Hio		houtb;
static	HConnect	*connect;

static	char		Top[] = "/usr/web/notas";

void	doconvert(char*, int);

void
error(char *fmt, ...)
{
	va_list arg;
	char buf[1024], *out;

	va_start(arg, fmt);
	out = vseprint(buf, buf+sizeof(buf), fmt, arg);
	va_end(arg);
	*out = 0;

	hprint(hout, "%s 404 %s\n", hversion, buf);
	hprint(hout, "Date: %D\n", time(nil));
	hprint(hout, "Server: Plan9\n");
	hprint(hout, "Content-type: text/html\n");
	hprint(hout, "\n");
	hprint(hout, "<head><title>%s</title></head>\n", buf);
	hprint(hout, "<body><h1>%s</h1></body>\n", buf);
	hflush(hout);
	writelog(connect, "Reply: 404\nReason: %s\n", buf);
	exits(nil);
}

int
isnif(char* nif)
{
	int	i, l;

	l = strlen(nif);
	if (l < 7 || l > 15)
		return 0;
	for (i = 0; i < l-1; i++)
		if (!isdigit(nif[i]))
			return 0;
	if (!isupper(nif[i]))
		return 0;
	return 1;
}

int
isexp(char* nif)
{
	int	i, l;

	l = strlen(nif);
	if (l != 4)
		return 0;
	for (i = 0; i < l; i++)
		if (!isdigit(nif[i]))
			return 0;
	return 1;
}

void
notanif(char* nif)
{
	hprint(hout, "<head><title>Notas</title></head>\n");
	hprint(hout, "<body>\n");
	hprint(hout, "<p>El NIF o No. de expediente que has inidicado (%s) <b>no es valido</b>.\n", nif);
	hprint(hout, "Ha de ser de la forma 0394834L si es un NIF.\n");
	hprint(hout, "Con al menos 6 digitos y una letra final.\n");
	hprint(hout, "<p>El formato ha de ser exactamente el mismo que diste\n");
	hprint(hout, "para tu matriculacion.\n");
	hprint(hout, "Si es un No. de expediende ha de ser como 1234.\n");
	hprint(hout, "</body>\n");
}

void
notfound(void)
{
	hprint(hout, "<head><title>Notas</title></head>\n");
	hprint(hout, "<body>\n");
	hprint(hout, "<p>El NIF que has inidicado <b>no esta en la lista</b>.\n");
	hprint(hout, "<p>El formato ha de ser exactamente el mismo que diste\n");
	hprint(hout, "para tu matriculacion.\n");
	hprint(hout, "</body>\n");
}

char*
dumphdr(char* exam)
{
	Biobuf	b;
	char	fname[255];
	int	fd;
	char*	p;
	char*	last;
	static char empty[] = "";

	seprint(fname, fname+sizeof(fname), "%s/%s.hdr", Top, exam);
	fd = open(fname, OREAD);
	if (fd < 0)
		return empty;
	Binit(&b, fd, OREAD);
	last = nil;
	for(;;){
		p = Brdline(&b, '\n');
		if(p == nil)
			break;
		if (last != nil){
			hprint(hout, "%s\n", last);
			free(last);
		}
		p[Blinelen(&b)-1] = 0;
		last = strdup(p);
	}
	close(fd);
	if (last == nil)
		return empty;
	else
		return last;
}

char*
lookup(char* nif, char* exam)
{
	Biobuf	b;
	char	fname[255];
	int	fd;
	int	l;
	char*	p;

	seprint(fname, fname+sizeof(fname), "%s/%s", Top, exam);
	fd = open(fname, OREAD);
	if (fd < 0)
		error("The exam (%s) does not exist.", fname);
	Binit(&b, fd, OREAD);
	for(;;){
		p = Brdline(&b, '\n');
		if(p == nil)
			break;
		if (Blinelen(&b) < 20)	// line too short to be useful.
			continue;
		p[Blinelen(&b)-1] = 0;
		l = strlen(nif);
		if (strncmp(p, nif, l) == 0 && isspace(p[l])){
			close(fd);
			return strdup(p);
		}
	}
	close(fd);
	return nil;
}


void
output(char* hdr, char* line)
{
	char*	h[50];
	int	nh;
	char*	l[50];
	int	nl;
	char*	ohdr;
	char*	oline;
	int	i;

	ohdr = strdup(hdr);
	oline= strdup(line);

	nh = getfields(hdr, h, nelem(h), 1, "\t,");
	nl = getfields(line, l, nelem(l), 1, "\t,");

	if (nh != nl){
		//fprint(2, "nh %d nl %d\n", nh, nl);
		// Things dont match, do what we can.
		hprint(hout, "<pre>\n");
		hprint(hout, "%s\n", ohdr);
		hprint(hout, "%s\n", oline);
		hprint(hout, "</pre>\n");
	} else {
		hprint(hout, "<table cellspacing=10>\n<tr  align=left>");
		for (i = 0; i < nh; i++)
			hprint(hout, "<td>%s</td> ", h[i]);
		hprint(hout, "</tr>\n<tr>");
		for (i = 0; i < nl; i++)
			hprint(hout, "<td>%s</td> ", l[i]);
		hprint(hout, "</tr></table>\n");
	}
	
}

void
notas(char *nif, char* exam, int vermaj)
{
	char *line;
	char  *hdr;
	if(nif == nil || nif[0] == 0)
		error("you must specify your NIF");
	if(exam == nil || exam[0] == 0)
		error("you must specify a exam");
	line = lookup(nif, exam);
	if(vermaj){
		hokheaders(connect);
		hprint(hout, "Content-type: text/html\r\n");
		hprint(hout, "\r\n");
	}
	if(!isnif(nif) && !isexp(nif)){
		notanif(nif);
		return;
	}
	if (line == nil){
		notfound();
		return;
	}
	hprint(hout, "<head><title>Listado de Notas</title></head><body bgcolor=\"white\">\n");
	hprint(hout, "<H1>Notas de %s en %s</H1>\n<hr>\n<p>\n<pre>\n", nif, exam);
	hdr = dumphdr(exam);
	hprint(hout, "</pre><p>\n");
	output(hdr, line);
	hprint(hout, "<hr><p>\n</body>\n");
}

void
dosearch(int vermaj, char* search)
{
	char*	nif;
	char*	exam;

	search = hurlunesc(connect, search);
	if(strncmp(search, "nif=", 4) != 0)
		error("search does not set NIF");
	nif = search+4;
	exam = strchr(nif, '&');
	if(exam == nil)
		error("search does not set exam");
	*exam++ = 0;
	if(strncmp(exam, "exam=", 5) == 0)
	exam = exam+5;
	notas(nif, exam, vermaj);
}

void
main(int argc, char **argv)
{
	fmtinstall('H', httpfmt);
	fmtinstall('U', hurlfmt);

	//dup(open("/dev/cons", OWRITE), 2);
	close(2);
	connect = init(argc, argv);
	hout = &connect->hout;
	if(hparseheaders(connect, HSTIMEOUT) < 0)
		exits("failed");

	if(strcmp(connect->req.meth, "GET") != 0 && strcmp(connect->req.meth, "HEAD") != 0){
		hunallowed(connect, "GET, HEAD");
		exits("not allowed");
	}
	if(connect->head.expectother || connect->head.expectcont){
		hfail(connect, HExpectFail, nil);
		exits("failed");
	}

	if(connect->req.search != nil)
		dosearch(connect->req.vermaj, connect->req.search);
	else
		error("You should use our form to search for notas.");
	hflush(hout);
	writelog(connect, "200 nota2html\n");
	exits(nil);
}
