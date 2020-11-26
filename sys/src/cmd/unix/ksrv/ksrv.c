#include <u.h>
#include <libc.h>
#include <auth.h>
#include <thread.h>
#include "ksrv.h"
#include <X11/keysym.h>
#include "keycodes.h"

int mainstacksize = 15*1024;
int verbose = 0;
int debug = 0;
int doauth;



char *addr;
enum{	
	Rreclaim	= 0xef80810a	// rune to reclaim kbd (F1)
};

static void
usage(void)
{
	fprint(2, "usage: kbdsrv [-A] [-d] [-v] [-s] address \n");
	threadexits("usage");
}

static Key *
decode(char *msg)
{
	Key *k;
	Rune r;

	k = malloc(sizeof(Key));
	memset(k, 0, sizeof(Key));
	memcpy(k->orig, msg, sizeof(int));
	chartorune(&r,msg);
	k->rune = r;
	return(k);
}

static int
authfd(int fd, char *role)
{
	AuthInfo *i;
	
	i = nil;
	alarm(5*100);	//BUG this just dies.
	USED(i);
	i = auth_proxy(fd, nil, "proto=p9any user=%s role=%s", getuser(), role);
	alarm(0);
	if(i == nil)
		return 0;
	auth_freeAI(i);
	return 1;
}

static int
alarmed(void *v, char *s)
{
	USED(v);
	if(strcmp(s, "alarm") == 0){
		fprint(2, "timeout sending event\n");
		return 1;
	}
	return 0;
}

Channel *senderc;

static void
sender(void *v)
{
	Key *rkey;
	int res;
	USED(v);
	
	while(rkey = (Key *)recvp(senderc)){
		alarm(5*1000);
		res = sendkey(rkey);
		if(!res){
			fprint(2, "error sending movement %r\n");			
			alarm(5*1000);
			closedisplay();	//buggy, race condition, but last chance.
			alarm(0);
			alarm(5*1000);
			if(initdisplay() < 0)
				sysfatal("problem reopening display");
			sendkey(rkey);
		}
		alarm(0);
		if(verbose) 
			fprint(2, "key: [%C] 0x%x, keycode: 0x%x %d %d %d\n", 
				rkey->rune, rkey->rune, rkey->keycode, 
				rkey->isshift, rkey->iscontrol, rkey->isaltgr);
		free(rkey);
	}
}

static void
attend(void *v)
{
	int rd, dfd;
	char	msg[50];
	Key *rkey;


	dfd=(int)v;

	threadnotify(alarmed, 1);

	for(;;){
		
		rd=read(dfd,msg,sizeof(msg)-1);
		
		if(rd<0){
			if(verbose) fprint(2, "ksrv: error reading %r\n");
			sysfatal("Error reading %r");
		}
		if(rd==0){
			if(verbose) fprint(2, "ksrv: closed connection\n");
			break;
		}
		msg[rd] = '\0';
		rkey=decode(msg);
		if(!rkey){			//on error, close connection
			fprint(2, "ksrv: error decoding, n=%d, %x, [%s]\n", rd, 
				msg[0], msg);
			break;
		}

		if(!rkey->rune){
			if(verbose)
				fprint(2, "0 rune received, closing\n");
			break;
		}
		sendp(senderc, (void *)rkey);
		
	}
	close(dfd);
	if(verbose) print("Exiting\n");
	threadexits(nil);
}



static void
mainproc(void *v)
{
	int	lfd, dfd;

	char	adir[40];
	char	ldir[40];
	USED(v);
	if(initdisplay() < 0)
		sysfatal("ksrv: error initializing display");
	if(announce(addr, adir)< 0)
		sysfatal("announce %s: %r", addr);
	for(;;){
		lfd = listen(adir, ldir);
		if (lfd < 0)
			sysfatal("can't listen: %r");
		dfd = accept(lfd, ldir);
		close(lfd);
		if(verbose) fprint(2, "ksrv: accepted connection\n");
		if(doauth && !authfd(dfd, "server")){
			fprint(2, "ksrv: auth failed");
			close(dfd);
		}
		else
			proccreate(attend, (void *)dfd, mainstacksize);
	}
	closedisplay();
}



extern int _threaddebuglevel;
void
threadmain(int argc, char **argv)
{
	doauth = 1;
	addr=nil;
	ARGBEGIN{
	default:
		usage();
	case 'v':
		verbose++;
		break;
	case 'd':
		debug++;
		break;
	case 'A':
		doauth=0;
		break;
	case 's':
		close(0);
		addr = EARGF(usage());
		break;
	}ARGEND


	if(!addr)
		addr=strdup("tcp!*!11001");

	if(verbose) fprint(2, "ksrv: running\n");

	if(verbose) fprint(2, "ksrv: forking\n");
	senderc = chancreate(sizeof(Key *), 0);
	if(!senderc)
		sysfatal("creating chan");

	proccreate(sender,nil,mainstacksize);
	proccreate(mainproc,nil,mainstacksize);
	if(verbose) fprint(2, "ksrv: exiting\n");
	threadexits(nil);

}

//BUG: X dependant, should probably go in its own file

void
translate(Key *k)
{
	int i;

	i = 0;
	while(ktab[i].r){
		if(ktab[i].r == k->rune){	
			if(verbose)
				print("ksrv: translated\n");
			k->keysym = ktab[i].keysym;
			k->isshift = ktab[i].isshift;
			k->iscontrol = ktab[i].iscontrol;
			xtranslate(k);
			return;
		}
		i++;
	}
	k->keysym = 0xFF&*(uint *)k->orig;	//heuristical translation
	k->isshift = isupperrune(k->rune);
	xtranslate(k);
	return;
}
