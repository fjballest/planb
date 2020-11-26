#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>
#include <plumb.h>
#include <omero.h>
#include <b.h>
#include "dat.h"

Window*
newwindow(char* name)
{
	Window*	w;
	char	n[50];

	dprint(2, "newwindow %s\n", name);
	w = emalloc(sizeof(Window));
	memset(w, 0, sizeof(*w));
	w->name = estrdup(name);
	w->g = createpanel("omail", "col", nil);
	seprint(n, n+sizeof(n), "tag:omail");
	w->gtag= createsubpanel(w->g, n);
	seprint(n, n+sizeof(n), "text:omail");
	w->gtext= createsubpanel(w->g, n);
	omeroeventchan(w->g);
	w->gtag->evc = w->g->evc;
	w->gtext->evc= w->g->evc;
	closepanelctl(w->g);
	return w;
}

int
windel(Window *w)
{
	/* event proc will die due to read error from event channel */
	removepanel(w->gtext);
	removepanel(w->gtag);
	removepanel(w->g);
	if (w->body){
		Bterm(w->body);
		remove(w->bodyf);
	}
	free(w->name);
	free(w);
	return 1;
}

void
wintagwrite(Window *w, char *s, int n)
{
	char	txt[80];

	seprint(txt, txt+sizeof(txt), "%s Done %.*s", w->name, n, s);
	openpanel(w->gtag, OWRITE|OTRUNC);
	writepanel(w->gtag, txt, strlen(txt));
	closepanel(w->gtag);
}

void
winclean(Window *w)
{
	winclosebody(w);
	openpanelctl(w->gtext);
	panelctl(w->gtext, "clean");
	closepanelctl(w->gtext);
}

void
winopenbody(Window *w)
{
	winclosebody(w);
	w->bodyf = smprint("/tmp/omail.%d.body", getpid());
	w->body = Bopen(w->bodyf, OWRITE|OCEXEC);
	if(w->body == nil)
		error("can't open window body file: %r");
}

void
winclosebody(Window *w)
{
	char*	s;

	if(w->body != nil){
		Bflush(w->body);
		s = readfstr(w->bodyf);
		openpanel(w->gtext, OWRITE|OTRUNC);
		writepanel(w->gtext, s, strlen(s));
		closepanel(w->gtext);
		free(s);
		openpanelctl(w->gtext);
		panelctl(w->gtext, "clean\nsel 0 0\n");
		closepanelctl(w->gtext);
		Bterm(w->body);
		w->body = nil;
		free(w->bodyf);
	}
}

void
winwritebody(Window *w, char *s, int n)
{
	if(Bwrite(w->body, s, n) != n)
		error("write error to window: %r");
}

char*
winreadbody(Window *w, int *np)
{
	long	l;
	char*	s;

	if(w->body != nil)
		winclosebody(w);
	s = readallpanel(w->gtext, &l);
	*np = l;
	return s;
}

