#include <stdio.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "msrv.h"

//needs libtst-dev y xlibs-dev
//No grabbing, conflict with wm.

Display *dpy;
enum{
	Delay = 1
};

int
initdisplay()
{
	dpy = XOpenDisplay( NULL );		//for the moment default
	if(!dpy)
		return -1;
	return 0;
}

//BUG: should I free dpy??

int
closedisplay()
{
	return XCloseDisplay( dpy );
}

int
mousedown(int button)
{

    return XTestFakeButtonEvent( dpy, button, True, Delay );
}

int
mouseup(int button)
{
	return XTestFakeButtonEvent( dpy, button, False, Delay );
}


int
mouseclick(int button)
{
	int res;
	res = XTestFakeButtonEvent( dpy, button, True, Delay );
	res = res && XTestFakeButtonEvent( dpy, button, False, Delay );
	return (res);
}


int
mousemove(int x, int y)
{
	return XTestFakeMotionEvent( dpy, -1, x, y, Delay );
}

int
sendmov(Mov *m)
{
	int i;
	XTestGrabControl(dpy, True);
	if(!mousemove(m->x, m->y))
		return -1;
	for(i = 0; i<5; i++){
		switch(m->but[i]){
			case Nothing:
				break;
			case Click:
				if(!mouseclick(i+1))
					return -1;
				break;
			case Down:
				if(!mousedown(i+1))
					return -1;
				break;
			case Up:
				if(!mouseup(i+1))
					return -1;
				break;
		}
	}
	XFlush(dpy);
	return 0;
}
