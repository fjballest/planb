#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include "article.h"

Post*
newpost(void)
{
	Post*	p;

	p = malloc(sizeof(Post));
	memset(p, 0, sizeof(p));
	return p;
}

void
closepost(Post* p)
{
	if (p == nil)
		return;
	free(p->title);
	free(p->author);
	free(p->body);
	free(p);
}

int
prefixcmp(int top[], int p[], int n)
{
	int	i;

	for (i = 0; i < Nids && i < n; i++)
		if (top[i] != p[i])
			return 1;
	return 0;
}

char*
id2str(int ids[], int n)
{
	char buf[100];
	char*	s;
	int	i;

	s = buf;
	for (i = 0; i < n; i++)
		if (i == 0)
			s = seprint(s, buf+sizeof(buf), "%d", ids[i]);
		else
			s = seprint(s, buf+sizeof(buf), ".%d", ids[i]);
	return strdup(buf);
}

int
parseid(char* buf, int ids[], int max)
{
	char*	toks[Nids+1];
	int	i;
	int	n;

	n = getfields(buf, toks, nelem(toks), 0, ".");
	if (n > Nids || n > max)
		n = 0;
	for (i = 0; i < n; i++)
		ids[i] = atoi(toks[i]);
	return n;
}

int
isson(Post* t, Post* p)
{
	if (t != nil && p != nil && t->nids == p->nids - 1)
	if (!prefixcmp(t->ids, p->ids, t->nids))
		return 1;
	return 0;
}


	
char*
seprintpost(char* s, char* e, Post* p)
{
	char*	is;

	is = id2str(p->ids, p->nids);
	s = seprint(s, e, "%s", is);
	free(is);
	s = seprint(s, e, "\n%s\n", p->title);
	s = seprint(s, e, "%s\n", p->author);
	return seprint(s, e, "%s%c", p->body, Eop);
}



Post*
readpost(Biobuf* b)
{
	Post*	p;
	char*	ln;

	ln = Brdstr(b, '\n', 1);
	if (ln == nil)
		return nil;

	p = malloc(sizeof(Post));
	memset(p, 0, sizeof(Post));
	p->nids = parseid(ln, p->ids, Nids);
	free(ln);
	p->title = Brdstr(b, '\n', 1);
	p->author= Brdstr(b, '\n', 1);
	p->body = Brdstr(b, Eop, 1);
	if (p->nids == 0 || !p->title || !p->author || !p->body){
		fprint(2, "null post member\n");
		closepost(p);
		return nil;
	} else
		return p;
}

/*
 * Top ids[] is a prefix of p ids[]
 * we descend evaluating more into the prefix
 * until we have a single element to evaluate
 */
void
insert(Post* top, Post* p)
{
	int	i;
	Post*	s;
	Post*	nxt;

	while(!isson(top, p)){
		nxt = nil;
		for (i = 0; i < top->nsons; i++){
			s = top->sons[i];
			if (s != nil)
			if (!prefixcmp(s->ids, p->ids, s->nids)){
				nxt = s;
				break;
			}
		}
		if (nxt == nil){
			fprint(2, "warning: parent %s\n", top->title);
			fprint(2, "warning: son w/o parent %s\n", p->title);
			return;
		}
		top = nxt;
	}
	if (top->nsons == Nsons){
		fprint(2, "warning: Nsons reached\n");
		return;
	}
	top->sons[top->nsons++] = p;
}

Post*
lookup(Post* p, int ids[], int nids)
{
	int	i;
	Post*	s;
	Post*	nxt;


	while(p != nil && !(p->nids == nids && !prefixcmp(p->ids, ids, nids))){
		nxt = nil;
		for (i = 0; i < p->nsons; i++){
			s = p->sons[i];
			if (s != nil)
			if (!prefixcmp(s->ids, ids, s->nids)){
				nxt = s;
				break;
			}
		}
		p = nxt;
	}
	return p;
}

Post*
readarticle(Biobuf* bin, int* n)
{
	Post*	top;
	Post*	p;

	if (n != nil)
		*n = 0;
	top = readpost(bin);
	while(top != nil){
		if (n != nil)
			*n = *n + 1;
		p = readpost(bin);
		if (p == nil)
			break;
		insert(top, p);
	}
	return top;
}
