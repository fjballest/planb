#include <u.h>
#include <libc.h>
#include <auth.h>
#include <thread.h>
#include "msrv.h"

int mainstacksize = 15*1024;
int verbose = 0;
int debug = 0;
int resx;
int resy;
int buttons;
int doauth;
int isreverse;

char *addr;



enum{
	Mevlen = 4*12+1,	//see mouse(2)
};

static int 
ispressed(int but, int bitfield)
{
	int mask = 0x1<<but;
	return mask&bitfield;
}

static void
usage(void)
{
	fprint(2, "usage: mouseserve [-A] [-x resx] [-y resy] [-v] [-s] [-r] address \n");
	threadexits("usage");
}

//BUG: doesnt work. ???
#pragma vargargck type "M" Mov*
int
Mfmt(Fmt *f)
{
	char *s;
	int res, i;
	Mov *m;
	m = va_arg(f->args, Mov*);
	s = malloc(512);
	if(!s)
		sysfatal("malloc mfmt");
	s[0] = '\0';
	
	s = seprint(s, s + 23, "%c ",  m->dev);
	for(i = 0; i<5; i++){
		s = seprint(s, s + 23, "%4.d ",  m->but[i]);
	}
	s = seprint(s, s + 23, "%5.d %5.d\n",m->x, m->y);
	s = seprint(s, s + 23, "isquit %5.d\n",m->isquit);
	s[511] = '\0';
	res = fmtprint(f,"%s",s);
	free(s);
	return res;
}


static Mov *
decode(char *msg)
{
	Mov *m;
	int n,i;
	char *args[4];
	int butread;
	n = tokenize(msg,args,nelem(args));
	if(n != nelem(args)){
		fprint(2,"Decoding error %s\n",msg);
		return nil;
	}
	m = malloc(sizeof(*m));
	if(!m)
		sysfatal("Memory alloc error %r\n");
	memset(m, 0, sizeof(Mov));
	m->dev = args[0][0];
	m->x = (atoi(args[1])*resx)/Stdxmax;
	m->y = (atoi(args[2])*resy)/Stdymax;

	butread = atoi(args[3]);
	if(!m->x && !m->y && !butread){	// spurious events
		if(verbose)
			fprint(2, "msrv: spurious event [%s][%s][%s]\n", args[0], args[1], args[2]);
		free(m);
		return nil;
	}

	for(i = 0;i<3;i++){ //normal buttons
		if(ispressed(i, butread) && !ispressed(i, buttons)){
			buttons |= 0x1<<i;
			m->but[i] = Down;
		}
		if(!ispressed(i, butread)&& ispressed(i, buttons)){
			buttons &= !(0x1<<i);
			m->but[i] = Up;
			if(m->x < 10 && m ->y < 10 && i==0){
				m->isquit = 1;
			}
		}
	}
	for(i = 3; i<5; i++){  //wheel may be changed in the future for nxtpage/prevpage
		if(ispressed(i, butread)){
			if(!isreverse)
				m->but[i] = Click;
			else {
				if(i == 3)
					m->but[4] = Click;
				else if(i == 4)
					m->but[3] = Click;
			}
		}
	}
	return m;
}

static int
authfd(int fd, char *role)
{
	AuthInfo *i;
	
	i = nil;
	alarm(5*100);		//BUG, this just dies.
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
		fprint(2, "timeout sending event");
		return 1;
	}
	return 0;
}


Channel *senderc;

static void
sender(void *v)
{
	Mov *m;
	int res;
	USED(v);
	
	while(m = (Mov *)recvp(senderc)){
		if(debug == 0){
			alarm(5*1000);
			res = sendmov(m);
			if(res < 0){
				fprint(2, "error sending movement %r");			
				alarm(5*1000);
				closedisplay();
				alarm(0);
				if(initdisplay() < 0)
					sysfatal("problem reopening display");
				alarm(5*1000);
				sendmov(m);
			}
			alarm(0);
		}
		free(m);
	}
}


static void
attend(void *v)
{
	int rd, dfd;
	char	msg[Mevlen+1];
	Mov *m;

	dfd=(int)v;

	threadnotify(alarmed, 1);

	for(;;){
		//if(verbose)	fprint(2, "reading %d, from %d\n", sizeof(msg)-1, dfd);
		
		rd = read(dfd,msg,sizeof(msg)-1);
		
		if (rd < 0){
			if(verbose) fprint(2, "error reading %r\n");
			sysfatal("Error reading %r");
		}
		if(rd == 0){
			if(verbose) fprint(2, "closed connection\n");
			break;
		}
		msg[rd] = '\0';
		if(verbose) 
			fprint(2, "MSG, %d: %s\n", rd, msg);
		if(strncmp(msg, "bye", 3) == 0){
			if(verbose) 
				fprint(2, "MSG: %s\n", msg);
			break;
		}
		if(strncmp(msg, "call ", 5) == 0)
			continue;		//ignore calls
		m = decode(msg);
		if(!m)
			continue;
		/*if(verbose) 
			fprint(2, "%M\n", m); BUGGY*/
		if(m->isquit){
			if(verbose)
				print("pointed to corner");
			free(m);
			alarm(5*1000);
			write(dfd, "bye", strlen("bye"));
			alarm(0);
			break;
		}
		sendp(senderc, m);
	}
	close(dfd);
	if(verbose) print("Exiting\n");
	threadexits(nil);
}



static void
mainproc(void *v)
{
	int	afd, lfd, dfd;

	char	adir[40];
	char	ldir[40];

	if((afd = announce(addr, adir)) < 0)
		sysfatal("announce %s: %r", addr);

	for(;;){
		lfd = listen(adir, ldir);
		if (lfd < 0)
			sysfatal("can't listen: %r");
		dfd = accept(lfd, ldir);
		close(lfd);
		if(verbose) fprint(2, "accepted connection\n");
		if(doauth && !authfd(dfd, "server")){
			fprint(2, "msrv: auth failed");
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
	buttons = 0;
	addr = nil;
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
		addr = EARGF(usage());
		break;
	case 'x':
		resx = atoi(EARGF(usage()));
		break;
	case 'y':
		resy = atoi(EARGF(usage()));
	break;
	case 'r':
		isreverse++;
		break;
	}ARGEND

	if(!debug)
		close(0);
	if((resx <= 0) || (resy <= 0)){
		resx = 1024;
		resy= 768;
	}
	if(!addr)
		addr = strdup("tcp!*!11000");

	if(verbose) fprint(2, "mouseserve running\n");

	fmtinstall('M',Mfmt);
	if(initdisplay() < 0)
		sysfatal("Error opening display");
	if(verbose) fprint(2, "mouse forking\n");

	senderc = chancreate(sizeof(Mov *), 0);
	if(!senderc)
		sysfatal("creating chan");

	proccreate(sender,nil,mainstacksize);
	proccreate(mainproc, nil, mainstacksize);
	if(verbose) fprint(2, "msrv exiting\n");
	threadexits(nil);
}

