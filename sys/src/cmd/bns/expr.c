/*
 * Constraints and expressions.
 */

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <ip.h>
#include "names.h"
#include "vols.h"


static char**	atoms;
static int	natoms;
Cnstr	netokcnstr;
Cnstr	netbadcnstr;

static int
atom(char* a)
{
	int	i;

	if(a == nil){
		fprint(2, "%s: atom: nil atom\n", argv0);
		return atom("");
	}
	for (i = 0; i < natoms; i++)
		if (!strcmp(atoms[i], a))
			return i;
	if (!(natoms%Incr))
		atoms = erealloc(atoms, (natoms+Incr)*sizeof(char*));
	atoms[natoms] = estrdup(a);
	natoms++;
	return natoms - 1;
}


void
exprfree(Expr* e)
{
	int	i;

	if (e != nil){
		for (i = 0; i < e->ncnstrs; i++)
			free(e->cnstrs[i]);
		free(e);
	}
}

Cnstr*
parsecnstr(char* ln, Cnstr* c)
{
	char*	toks[Nattrs];
	int	ntoks;
	char*	p;
	int	na;
	int	i;
	char*	nln;

	nln = estrdup(ln);
	ntoks = gettokens(nln, toks, nelem(toks), " \t");
	if (ntoks == 0){
		free(nln);
		return nil;
	}
	if (ntoks == Nattrs)
		fprint(2, "%s: too many attrs in constraint: %s.\n", argv0, ln);
	if (c == nil)
		c = emalloc(sizeof(Cnstr));
	c->nattrs = 0;
	for (i = 0; i < ntoks; i++){
		p = strchr(toks[i], '=');
		if (p == nil){
			fprint(2, "%s: bad attribute: %s: %s\n", argv0, ln, toks[i]);
			continue;
		}
		*p++ = 0;
		if (*toks[i] == 0 || *p == 0){
			*--p = '=';
			fprint(2, "%s: bad attribute: %s: %s\n", argv0, ln, toks[i]);
			continue;
		}
		na = c->nattrs++;
		c->attrs[na].var = atom(toks[i]);
		c->attrs[na].val = atom(p);
	}
	free(nln);
	return c;
}

Expr*
parseexpr(char* ln, Expr* e)
{
	char*	buf;
	char*	p;
	char*	np;
	Cnstr*	c;

	e->ncnstrs = 0;

	if (ln == nil)
		ln = "";
	buf = estrdup(ln);
	for (p = buf; p && *p; p = np){
		if (e->ncnstrs >= Ncnstrs){
			fprint(2, "%s: too many constraints in spec\n", argv0);
			break;
		}
		np = strchr(p, '|');
		if (np)
			*np++ = 0;
		c = parsecnstr(p, nil);
		if (c && c->nattrs > 0)
			e->cnstrs[e->ncnstrs++] = c;
		else
			free(c);
	}
	free(buf);
	return e;
}

int
cnstrmatch(Cnstr* env, Cnstr* c)
{
	int	i, j;
	Prop*	a;

	if (c == nil)
		return 1;
	for (i = 0; i < c->nattrs; i++){
		if (env == nil)
			return 0;
		a = &c->attrs[i];
		for (j = 0; j < env->nattrs; j++){
			if (a->var == Any || a->var == env->attrs[j].var)
			if (a->val == Any || a->val == env->attrs[j].val)
				break;
		}
		if (j == env->nattrs)
			return 0;
	}
	return i == c->nattrs;
}

void
cnstrcat(Cnstr* c1, Cnstr* c2)
{
	int i, j;

	if (c1 == nil || c2 == nil)
		return;
	for (j = 0; j < c2->nattrs; j++){
		for (i = 0; i < c1->nattrs; i++){
			if (c1->attrs[i].var == c2->attrs[j].var){
				c1->attrs[i].val = c2->attrs[j].val;
				break;
			}
		}
		if (i == c1->nattrs && c1->nattrs < Nattrs){
			c1->attrs[i] = c2->attrs[j];
			c1->nattrs++;
		}
	}
}


/* Returns -1 for no match.
 * Returns the index for the cnstr when matches
 */
int
exprmatch(Cnstr* env, Expr* exp)
{
	int	i;
	Cnstr*	c;

	if (exp->ncnstrs == 0)
		return 0;
	for (i = 0; i < exp->ncnstrs; i++){
		c = exp->cnstrs[i];
		if (cnstrmatch(env, c))
			return i;
	}
	return -1;
}

int
kfmt(Fmt* fmt)
{
	Cnstr*	c;
	Prop*	a;
	char	buf[128];
	char*	s;
	int	j;

	c = va_arg(fmt->args, Cnstr*);
	if (c == nil)
		return fmtstrcpy(fmt , "-");
	s = buf;
	*buf = 0;
	s = seprint(s, buf+sizeof(buf), "'");
	for (j = 0; j < c->nattrs; j++){
		a = &c->attrs[j];
		s = seprint(s, buf+sizeof(buf), "%s=%s ",
			atoms[a->var], atoms[a->val]);
	}
	seprint(s, buf+sizeof(buf), "'");
	return fmtstrcpy(fmt, buf);
}

int
Kfmt(Fmt* fmt)
{
	Expr*	e;
	char	buf[250];
	char*	s;
	int	i;

	e = va_arg(fmt->args, Expr*);
	if (e == nil || e->ncnstrs == 0)
		return fmtstrcpy(fmt , "-");
	s = buf;
	*buf = 0;
	for (i = 0; i < e->ncnstrs; i++){
		s = seprint(s, buf+sizeof(buf), "%k", e->cnstrs[i]);
		if (i < e->ncnstrs - 1)
			s = seprint(s, buf+sizeof(buf), "| ");
	}
	return fmtstrcpy(fmt, buf);
}

void
exprinit(void)
{
	atom("*");
	parsecnstr("net=ok",  &netokcnstr);
	parsecnstr("net=bad", &netbadcnstr);
}
