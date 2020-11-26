/* Automatically generated. Do not edit.
 */
#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <draw.h>
#include <mouse.h>
#include <ctype.h>
#include <keyboard.h>
#include <frame.h>
#include <9p.h>
#include <bio.h>
#include "gui.h"
#include "cook.h"

extern Pops nilops;
extern Pops rowops;
extern Pops colops;
extern Pops drawops;
extern Pops gaugeops;
extern Pops sliderops;
extern Pops imageops;
extern Pops pageops;
extern Pops textops;
extern Pops tagops;
extern Pops labelops;
extern Pops buttonops;
Pops* panels[] = {
	&nilops,
	&rowops,
	&colops,
	&drawops,
	&gaugeops,
	&sliderops,
	&imageops,
	&pageops,
	&textops,
	&tagops,
	&labelops,
	&buttonops,
	nil,
};

void
panelok(Panel* p)
{
	assert(p && p != (Panel*)0xfefefefe);
	assert(p->type >= 0 && p->type < nelem(panels));
}

