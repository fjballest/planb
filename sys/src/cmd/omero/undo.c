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
#include <ctype.h>
#include "gui.h"

int textdebug;

static void
dumpop(char* pref, Edit* e)
{
	int	nr;

	nr = e->nr > 3 ? 3 : e->nr;
	fprint(2, "%s%c%s %3d #%d\t [%.*S]\n", pref,
		e->end ? '*' : ' ',
		e->op == Ins ? "ins" : "del",
		e->pos, e->nr, nr, e->r);
}

static void
dumpedits(Panel* p)
{
	Edit*	e;

	fprint(2, "%s edits:\n", p->name);
	for (e = p->undos; e && e < p->undo; e++)
		dumpop("\t", e);
	fprint(2, "\n");
}

Edit*
addedit(Panel* p, int op, Rune* r, int nr, int pos)
{
	Edit*	e;
	int	nlen;
	Edit*	last;

	/* merge to succesive inserts into a single one, if feasible.
	 */
	if (op == Ins && p->undos && p->undo > p->undos){
		last = p->undo - 1;
		if (last->op == Ins && !last->end && pos == last->pos + last->nr){
			nlen = (nr + last->nr) * sizeof(Rune);
			last->r = erealloc9p(last->r, nlen);
			memmove(last->r + last->nr, r, nr*sizeof(Rune));
			last->nr += nr;
			return last;
		}
	}
	if (p->undo == p->undos+p->nundos){
		if (!(p->nundos%16))
			p->undos = erealloc9p(p->undos, (p->nundos+16)*sizeof(Edit));
		e = p->undos + p->nundos++;
		p->undo = p->undos + p->nundos;
	} else {
		assert(p->undo && p->undo < p->undos+p->nundos);
		e = p->undo++;
		free(e->r);
	}
	e->r = emalloc9p(nr * sizeof(Rune));
	memmove(e->r, r, nr*sizeof(Rune));
	e->nr = nr;
	e->pos= pos;
	e->op= op;
	e->end = 0;
	if (textdebug)
		dumpedits(p);
	return e;
}

void
newedit(Panel* p)
{
	Edit*	e;

	if (p->undo && p->undo > p->undos){
		e = p->undo-1;
		e->end = 1;
	}
}

int
hasedits(Panel* p)
{
	if (!p->undo)
		return 0;
	return (p->undo - p->undos) != p->cundo;
}

void
setnoedits(Panel* p)
{
	if (!p->undo)
		p->cundo = 0;
	else
		p->cundo = p->undo - p->undos;
}

void
cleanedits(Panel* p)
{
	int	i;

	for (i = 0; i < p->nundos; i++){
		free(p->undos[i].r);
	}
	free(p->undos);
	p->undos = p->undo = nil;
	p->nundos = 0;
	p->cundo = 0;
	if (textdebug)
		dumpedits(p);
}

static void
undo1(Panel* p)
{
	p->undo--;
	if (p->undo->op == Ins){
		textdel(p, p->undo->r, p->undo->nr, p->undo->pos);
		fillframe(p);
	} else
		textins(p, p->undo->r, p->undo->nr, p->undo->pos);
}

int
undo(Panel* p)
{
	Edit*	e;
	int	some;

	some = 0;
	while(p->undo && p->undo > p->undos){
		undo1(p);
		some++;
		if (p->undo > p->undos){
			e = p->undo - 1;
			if (e->end)
				break;
		}
	}
	return some;
}

static void
redo1(Panel* p)
{
	if (p->undo->op == Ins)
		textins(p, p->undo->r, p->undo->nr, p->undo->pos);
	else {
		textdel(p, p->undo->r, p->undo->nr, p->undo->pos);
		fillframe(p);
	}
	p->undo++;
}

int
redo(Panel* p)
{
	Edit*	e;
	int	last;
	int	some;

	e = p->undos + p->nundos;
	for(last = some = 0; !last && p->undo && p->undo < e; some++){
		last = p->undo->end;
		redo1(p);
	}
	return some;
}
