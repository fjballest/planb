#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <draw.h>
#include <mouse.h>
#include <cursor.h>
#include <keyboard.h>
#include <frame.h>
#include <9p.h>
#include "gui.h"

/* CAUTION:
 * Tblock routines always measures sizes in runes.
 * Below, pos means a position measured in runes,
 * buf off means an offset measured in (UTF8) bytes.
 *
 * Iteration is usually done with blookseek and
 * routines accepting block, n, ... args. There, n
 * is the position within a Tblock node.
 * For complex navigation, packblock() ensures that
 * all the block is contiguous.
 */

enum {
	Ndump = 30,
	Nmin = 1024,
};

int blockdebug;

int
Tfmt(Fmt* f)
{
	Tblock*	b;

	b = va_arg(f->args, Tblock*);
	if (b == nil)
		return fmtprint(f, "<nil block>\n");
	else{
		return fmtprint(f, "block: %d of %d runes [%.*S%s]\n",
			b->nr, b->ar, b->nr > Ndump ? Ndump : b->nr, b->r,
			b->nr > Ndump ? "..." : "");
	}
}

void
blockdump(Tblock* b)
{
	int	i;

	for(i = 0; b; i++){
		print("%p:%d %T\n", b->block0, i, b);
		b = b->next;
	}
	print("\n");
}

	
Tblock*
newblock(Rune* r, int nr)
{
	Tblock*	b;

	b = emalloc9p(sizeof(Tblock));
	memset(b, 0, sizeof(Tblock));
	b->block0 = b;
	if (r && !nr){
		free(r);
		r = nil;
	}
	if (r){
		b->r = r;
		b->nr= b->ar = nr;
	} else {
		b->nr = 0;
		b->ar = Nblock;
		b->r = emalloc9p(Nblock*sizeof(Rune));
	}
	return b;
}

void
blockfree(Tblock* b)
{
	Tblock*	nb;

	while(b){
		nb = b;
		b = b->next;
		assert(nb->ar || nb->r == nil);
		free(nb->r);
		free(nb);
	}
}

long
blocklen(Tblock* b)
{
	long	l;

	l = 0;
	while(b){
		assert(b->nr <= b->ar);
		l += b->nr;
		b = b->next;
	}
	return l;
}

void
packblock(Tblock* b)
{
	Tblock*	nb;
	long	l;

	if (b->next == nil)
		return;

	l = blocklen(b);
	if (l >= b->ar){
		b->r = erealloc9p(b->r, (l + Nblock) * sizeof(Rune));
		b->ar= l + Nblock;
	}
	for(nb = b->next; nb != nil; nb = nb->next){
		if (nb->nr > 0){
			memmove(&b->r[b->nr], nb->r, nb->nr*sizeof(Rune));
			b->nr += nb->nr;
		}
	}
	b->r[b->nr] = 0;
	blockfree(b->next);
	b->next = nil;
}

long
blocksize(Tblock* b)
{
	long	l;

	l = 0;
	while(b){
		if (b->nr)
			l += runenlen(b->r, b->nr);
		b = b->next;
	}
	return l;
}

long
pos2off(Tblock* b, long pos)
{
	long	n, i;

	if (pos < 0)
		return pos;
	for(n = 0; pos > 0 && b != nil; b = b->next)
		for (i = 0; pos > 0 && i < b->nr; i++){
			n += runelen(b->r[i]);
			pos--;
		}
	return n;
}

long
off2pos(Tblock* b, long off)
{
	long	n, i;
	
	if (off < 0)
		return off;
	for(n = 0; off > 0 && b != nil; b = b->next)
		for (i = 0; off > 0 && i < b->nr; i++){
			off -= runelen(b->r[i]);
			n++;
		}
	return n;
}

/* It's ok to seek to blocklen(b). That places us
 * right after the last valid rune.
 * It's not ok to seek past that value.
 */
Tblock*
blockseek(Tblock* b, int *np, int off)
{
	Tblock**lb;

	assert(off >= 0);
	bdprint("blockseek %d →", off);
	lb = nil;
	do {
		if (b->nr == 0 && lb){
			*lb = b->next;
			free(b->r);
			free(b);
			b = *lb;
			continue;
		} 
		if (off <= b->nr)
			break;
		off -= b->nr;
		lb = &b->next;
		b = b->next;
	} while(b);
	*np = off;
	while(b && lb && b->nr == 0){
		*lb = b->next;
		free(b->r);
		free(b);
		b = *lb;
	}
	assert(!b || (*np >= 0 && *np <= b->nr));	// i == b->nr is ok.
	bdprint("#%d of %T\n", off, b);
	return b;
}

static void
_blockget(Tblock* b, int off, int len, Rune* rp, int del)
{
	int	edel;
	int	ndel;
	int	n;

	assert(off >= 0 && off <= b->ar);
	bdprint("_blockget %d %d from %T\n", off, len, b);
	do {
		edel = off + len;
		assert(edel > 0);
		if (edel > b->nr)
			edel = b->nr;
		n = b->nr - edel;
		ndel = edel - off;
		if (ndel > 0 && rp){
			memmove(rp, b->r + off, ndel*sizeof(Rune));
			rp += ndel;
		}
		if (del){
			if (n > 0)
				memmove(b->r + off, b->r + edel, n*sizeof(Rune));
			b->nr -= ndel;
		}
		len -= ndel;
		off = 0;
		b = b->next;
	} while(b && len > 0);
}

void
blockget(Tblock* b, int n, int len, Rune* rp)
{
	if (len)
		_blockget(b, n, len, rp, 0);
}

static Rune*
putinblock(Tblock* b, int off, Rune* r, int len)
{
	int	ntrail;

	assert(b->nr + len <= b->ar);
	ntrail = b->nr - off;
	if (ntrail > 0)
		memmove(b->r + (off + len), b->r + off, ntrail*sizeof(Rune));
	memmove(b->r + off, r, len*sizeof(Rune));
	b->nr += len;
	return r + len;
}

static void
split(Tblock* b, int off)
{
	Tblock*	t;
	Rune*	r;
	int	nr;

	assert(off >= 0 && off <= b->ar);
	if (b->nr > off){
		nr = b->nr - off;
		if (nr < Nmin)
			nr = Nmin;
		r = emalloc9p(nr * sizeof(Rune));
		memmove(r, b->r + off, (b->nr - off) * sizeof(Rune));
		t = newblock(r, b->nr - off);
		t->ar = nr;
		t->block0 = b->block0;
		b->nr = off;
		t->next = b->next;
		b->next = t;
	}
}

void
blockins(Tblock* b, int off, Rune* r, int len)
{
	int	n;
	Tblock*	t;
	Rune*	ar;
	int	an;

	assert(off >= 0 && off <= b->nr);
	bdprint("blockins %d [%.*S] into %T\n", off, b->nr, r, b);
	assert(len > 0);
	if (b->nr + len <= b->ar){
		putinblock(b, off, r, len);
		return;
	}
	split(b, off);
	do {
		n = len;
		if (b->nr == 0 && b->ar){
			if (n > b->ar)
				n = b->ar;
			r = putinblock(b, 0, r, n);
		} else {
			if (n < Nmin)
				an = Nmin;
			else
				an = n;
			ar = emalloc9p(an * sizeof(Rune));
			memmove(ar, r, n * sizeof(Rune));
			t = newblock(ar, n);
			t->ar = an;
			t->block0 = b->block0;
			t->next = b->next;
			b->next = t;
			b = t;
		}
		len -= n;
	} while(len > 0);
}

void
blockcat(Tblock* b, Tblock* nb)
{
	blockfree(b->next);
	b->next = nb;
	while(nb){
		nb->block0 = b->block0;
		nb = nb->next;
	}
}

void
blockdel(Tblock* b, int n, int len, Rune* rp)
{
	_blockget(b, n, len, rp, 1);
}

Tblock*
str2block(char* s)
{
	Rune*	r;
	int	nr;

	r = utf2runes(s, nil, nil);
	nr= runeslen(r);
	return newblock(r, nr);
}

char*
block2str(Tblock* bl, long* l)
{
	char*	s;
	char*	bs;
	int	n;
	Tblock*	b;
	int	i;
	long	nc, nr;

	n = blocksize(bl);
	s = malloc(n+1);
	s[n] = 0;
	bs = s;
	nc = 0;
	for(b = bl; b != nil; b = b->next){
		for (i = 0; i < b->nr; i++){
			nr = runetochar(bs, b->r+i);
			bs += nr;
			nc += nr;
		}
	}
	*bs = 0;
	if (l)
		*l = nc;
	return s;
}

Rune*
utf2runes(char* s, int* lp, int* nlines)
{
	Rune*	r;
	int	n;
	int	l;
	int	wc;
	char*	se;
	int	lc;

	if (!lp)
		l = utflen(s);
	else {
		if (*lp)
			l = utfnlen(s, *lp);
		else
			l = utflen(s);
		*lp = 0;
	}
	if (nlines)
		*nlines = 0;
	r = emalloc9p(sizeof(Rune)*(l+1));
	setmalloctag(r, getcallerpc(&s));
	se = s;
	lc = 0;
	for(n = 0; n < l; n++){
		wc = chartorune(r+n, se);
		se += wc;
		if ((r[n] == '\n' || ++lc > 80) && nlines){
			lc = 0;
			(*nlines)++;
		}
	}
	r[l] = 0;
	if (lp)
		*lp = se - s;
	return r;
}

int
runeslen(Rune* r)
{
	int	n;

	for (n = 0; *r; r++, n++)
		;
	return n;
}
