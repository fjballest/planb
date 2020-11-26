#include <stdio.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "ksrv.h"

//needs libtst-dev y xlibs-dev
//No grabbing, conflict with wm.

Display *dpy;
enum{
	Delay = 1,
	Modstart = 4,
};

XModifierKeymap *xm;
int shiftidx;	//this could be done sending xm->keycode directly
int altgridx;
int controlidx;
KeySym altgrxk = XK_Mode_switch;

int
initdisplay()
{
	int i;
	KeySym modif;
	//test query extension could be called...
	dpy = XOpenDisplay( NULL );		//for the moment default
	if(!dpy)
		return -1;
	xm = XGetModifierMapping(dpy);
	if(!xm){
		 XCloseDisplay(dpy);
		return -1;
	}
	shiftidx = 1;
	altgridx = 2;
	controlidx = 0;
	
	for(i=0; i<8*xm->max_keypermod; i++){
		if(verbose)
			fprintf(stderr, "Modifier[%d]: 0x%x\n", 
				i/xm->max_keypermod+1, 
				(uint)XKeycodeToKeysym(dpy, xm->modifiermap[i], 0));
		switch(modif = XKeycodeToKeysym(dpy, xm->modifiermap[i], 0)){
			case XK_Mode_switch:
			case XK_ISO_Level3_Shift:
				altgrxk = modif;
				break;
			case XK_Shift_R:
			case XK_Shift_L:
				break;
			case XK_Control_R:
			case XK_Control_L:
				if(!controlidx)
					controlidx=i/xm->max_keypermod+1;
				break;
			default:
				break;
		}
	}
	if(verbose)
		fprintf(stderr, "ksrv: shift %d, control %d, altgr %d\n", shiftidx, controlidx, altgridx);
	return 0;
}

int
closedisplay()
{
	if(xm)
		XFreeModifiermap(xm);
	return XCloseDisplay( dpy );
}


int
sendkey(Key *k)
{
	int res;

	res = 1;
	k->keycode = 0;
	XTestGrabControl(dpy, True);
	

	translate(k);
	if(!k->keycode || !k->keysym){
		if(verbose) fprintf(stderr, "ksrv: error decoding\n");
		return -1;
	}

	if(!debug) {
		if(k->isaltgr){
			res = XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, altgrxk), True, CurrentTime);
			if(!res)
				return res;
		}
		if(k->iscontrol){
			res = XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Control_R), True, CurrentTime);
			if(!res)
				return res;
		}
		if(k->isshift){
			res = XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Shift_R), True, CurrentTime);
			if(!res)
				return res;
		}
		res = XTestFakeKeyEvent(dpy, k->keycode , True, CurrentTime);
		if(!res)
			return res;
		res = XTestFakeKeyEvent(dpy, k->keycode , False, CurrentTime);
		if(!res)
			return res;
		if(k->isshift){
			res = XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Shift_R), False, CurrentTime);
			if(!res)
				return res;
		}
		if(k->iscontrol){
			res = XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Control_R), False, CurrentTime);
			if(!res)
				return res;
		}
		if(k->isaltgr){
			res = XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, altgrxk), False, CurrentTime);
			if(!res)
				return res;
		}
		res = XFlush(dpy);
	}
	return res;
}

static void
applymod(Key *k, int idx)
{
	if(idx == 0){
		k->isshift = 0;
		k->iscontrol = 0;
		k->isaltgr = 0;
		return;
	}
	else if(idx == altgridx){
		k->isaltgr = 1;
		k->isshift = 0;
		k->iscontrol = 0;
		return;
	}
	else if(idx == controlidx){
		k->isshift = 0;
		k->iscontrol = 1;
		k->isaltgr = 0;
		return;
	}
	else if(idx == shiftidx){
		k->isshift = 1;
		k->iscontrol = 0;
		k->isaltgr = 0;
		return;
	}
}

void
xtranslate(Key *k)
{
	KeySym *xksltab;
	int i, nks;

	k->keycode = XKeysymToKeycode(dpy, k->keysym);

	/*
	Keysym revks;
	BUG of Xorgs makes this not work, careful with this function
	XKeycodeToKeysym(dpy, k->keycode, i);
	
	*/

	if(!k->keycode)
		return;
	xksltab = XGetKeyboardMapping(dpy, k->keycode, 1, &nks);
	for( i = 0; i < nks; i++){
		if(verbose)
			fprintf(stderr, "keysym: 0x%x, reverse[%d]: 0x%x\n", k->keysym, i, (uint)xksltab[i]);
		if(xksltab[i] == k->keysym)
			break;
	}

	if(verbose){
		fprintf(stderr, "The modifier is %d\n", i);
		fflush(stderr);
	}
	//I just permit one modifier
	applymod(k, i);
	
}
