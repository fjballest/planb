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
#include "cook.h"
#include "gui.h"

typedef struct Cooker Cooker;

enum {
	Tquiet	= 3000,	// timeout for quiet.
	Ttick	= 500,	// tick interval.
	Tdouble	= 500,	// timeout for double.
};

struct  Cooker{
	Channel*	m;	// raw.
	Channel*	c;	// cooked.
};

int
Mfmt(Fmt* f)
{
	Cmouse	*m;
	char*	s;

	m = va_arg(f->args, Cmouse*);
	switch(m->flag){
	case CMquiet:
		s = "q";
		break;
	case CMdouble:
		s = "d";
		break;
	default:
		s = " ";
	}
	return fmtprint(f," %s %x %P %uld", s, m->buttons, m->xy, m->msec);
}

static void
timerproc(void *a)
{
	Channel*c = a;

	threadsetname("timerproc");
	for(;;){
		sleep(Ttick);
		sendul(c, 0);
	}
}

static void
cookerthread(void* ca)
{
	Cooker*	cm = ca;
	Mouse	m, lm;
	Cmouse	c;
	ulong	last, now, pressmsec;
	int	dclick, pressb;
	int	nidle;
	Alt	a[] = {
		{ cm->m,	&m,	CHANRCV},
		{ nil,	&now,	CHANRCV},
		{ nil,	0,	CHANEND}};

	a[1].c = chancreate(sizeof(ulong), 5);
	proccreate(timerproc, a[1].c, 4*1024);
	pressmsec = last = time(nil);
	memset(&m, 0, sizeof(m));
	memset(&lm, 0, sizeof(lm));
	dclick = pressb = nidle = 0;
	for(;;){
		switch(alt(a)){
		case 0:
			if (0){	// do this for double buffering
				/* Adjust coords to window
				 */
				m.xy.x -= screen->r.min.x;
				m.xy.y -= screen->r.min.y;
			}
			/* Remove dups
			 */
			if (eqpt(lm.xy,m.xy) && lm.buttons == m.buttons)
				goto next;

			/* Insert release events between the press of
			 * different buttons.
			 */
			if (lm.buttons && m.buttons && !(lm.buttons&m.buttons)){
				c.Mouse = lm;
				c.buttons = 0;
				c.flag = 0;
				if (eventdebug > 2) edprint("sent1 %M\n", &c);
				send(cm->c, &c);
			}

			/* Flag double clicks.
			 */
			if (!lm.buttons && m.buttons){
				if (m.buttons == pressb){
					if (m.msec - pressmsec < Tdouble)
						dclick = 1;
				}
				pressb = m.buttons;
				pressmsec = m.msec;
			}
			c.flag = dclick ? CMdouble : 0;
			c.Mouse= m;
			if (!m.buttons && dclick){
				c.flag = CMdouble;
				dclick = 0;
			}
			if (eventdebug > 2) edprint("sent2 %M\n", &c);
			send(cm->c, &c);
		next:
			last = c.msec;
			lm = m;
			nidle = 0;
			break;

		case 1:
			/* send CMquiet when the mouse has been
			 * idle for more than Tquiet.
			 */
			if (lm.buttons)
				nidle = 0;
			else if (last && ++nidle > 3){
				c.Mouse = m;
				c.msec = last + 2 * Ttick;
				c.flag = CMquiet;
				if (eventdebug > 2) edprint("sent3 %M\n", &c);
				send(cm->c, &c);
				last = 0;
			}
			break;
		default:
			postnote(PNGROUP, getpid(), "mouse");
			sysfatal("mouse");
		}
	}
}

Channel*
cookmouse(Channel* mc)
{
	Cooker*	cm;

	cm = malloc(sizeof(Cooker));
	cm->m = mc;
	cm->c  = chancreate(sizeof(Cmouse), 16);
	threadcreate(cookerthread, cm, 8*1024);
	return cm->c;
}

int
cookclick(Cmouse* m, Channel* mc)
{
	int	b;

	b = m->buttons;
	do {
		recv(mc, m);
	} while(m->buttons == b);
	if (m->buttons == 0)
		return 1;
	do {
		recv(mc, m);
	} while(m->buttons != 0);
	return 0;
}

int
cookslide(Cmouse* m, Channel* mc)
{
	int	b;

	b = m->buttons;
	while(m->buttons == b){
		recv(mc, m);
	}
	if (m->buttons == 0)
		return 1;
	do {
		recv(mc, m);
	} while(m->buttons != 0);
	return 0;
}
