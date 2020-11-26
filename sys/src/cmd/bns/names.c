/*
 * Path names.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include "names.h"

enum {
	Incr = 128,
};

extern void* emalloc(long);
extern void* erealloc(void*, long);


Name*
n_new(void)
{
	Name*	n;

	n = emalloc(sizeof(Name));
	n->elems = emalloc(MAXWELEM * sizeof(char*));
	memset(n->elems, 0, MAXWELEM*sizeof(char*));
	n->aelems= MAXWELEM;
	n->nelems= 0;
	n->base = emalloc(Incr);
	n->end = n->base + Incr;
	n->ptr = n->base;
	*n->ptr = 0;
	return n;
}

int
namefmt(Fmt* f)
{
	Name*	n;
	char*	buf;
	int	bufsz;
	char*	s;
	int	i;
	int	r;

	n = va_arg(f->args, Name*);
	if (!n)
		return fmtprint(f, "nil name");
	bufsz = n->ptr - n->base + 1;
	if (bufsz == 1)
		bufsz++;	// be sure we can keep "/" at least
	buf = emalloc(bufsz);
	s = buf;
	s[0] = '/';
	s[1] = 0;
	for (i = 0; i < n->nelems; i++)
		s = seprint(s, buf + bufsz, "/%s", n->elems[i]);
	r =  fmtprint(f, "%s", buf);
	free(buf);
	return r;
}

void
n_reset(Name* n)
{
	n->nelems = 0;
	n->ptr = n->base;
}

void
n_append(Name* n, char* s)
{
	int	slen;
	int	nlen;
	int	alen;
	char*	nbase;
	int	i;

	slen = strlen(s);
	nlen = n->ptr - n->base;
	alen = n->end - n->base;
	while (alen < nlen + slen + 1){	// + 1 for zero
		nbase = erealloc(n->base, alen + Incr);
		alen += Incr;
		n->end = nbase + alen;
		n->ptr = nbase + nlen;
		for (i = 0; i < n->nelems && n->elems[i] != nil; i++)
			n->elems[i] = nbase + (n->elems[i] - n->base);
		n->base = n->base;
	}
	if (n->nelems == n->aelems){
		n->aelems += MAXWELEM;
		n->elems = erealloc(n->elems, n->aelems * sizeof(char*));
	}
	n->elems[n->nelems++] = n->ptr;
	strcpy(n->ptr, s);
	n->ptr += slen;
	*n->ptr++ = 0;
}

void
n_dotdot(Name* n)
{
	assert(n->nelems > 0);
	n->ptr = n->elems[--n->nelems];
}

void
n_getpos(Name* n, int* a, int* b)
{
	*a = n->nelems;
	*b = n->ptr - n->base;
}

void
n_setpos(Name* n, int a, int b)
{
	n->nelems = a;
	n->ptr = n->base + b;
	assert(n->ptr >= n->base && n->ptr < n->end);
}

void
n_cat(Name* cn, Name* n)
{
	int	i;

	for (i = 0; i < n->nelems; i++)
		n_append(cn, n->elems[i]);
}

int
n_eq(Name* n1, Name* n2)
{
	int	i;

	if (n1 == nil || n2 == nil)
		return n1 == n2;
	if (n1->nelems != n2->nelems)
		return 0;
	// paths use to differ near the end.
	// check it out in reverse order.
	for (i = n1->nelems-1; i >=0; i--)
		if (strcmp(n1->elems[i], n2->elems[i]))
			return 0;
	return 1;
}
	
void
n_free(Name* n)
{
	free(n->base);
	free(n->elems);
	free(n);
}
