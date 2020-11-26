#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <thread.h>
#include "x10.h"

typedef struct Event Event;	// event from CM11
typedef struct Func Func;	// function event
typedef struct CM11 CM11;	// CM11 status info

enum {
	Nevs	= 16,

	// Event.type
	Enone	= 0,
	Eaddr,
	Efunc,
};

struct Func {
	uchar	hc;	// house code
	uchar	fn;	// function
	union {
		// !Fext
		struct {
			uchar val;
		};
		// Fext
		struct {
			uchar	data;
			uchar	cmd;
		};
	};
};

struct Event {
	int	type;
	union {
		Addr	a;
		Func	f;
	};
};

struct CM11 {
	ushort	timer;
	short	h,m,s;
	short	day;
	short	yday;
	uchar	wdays;
	uchar	hc;
	uchar	vers;
	ushort	devs;
	ushort	sts;
	ushort	dim;	
};

struct X10 {
	char*	dev;		// serial device
	char*	sdev;		// ctl for device
	uchar	hc;		// house code
	int	fd;		// file descriptor for dev
	int	ndevs;		// # of known devices
	Dev	devs[Ndevs];	// known devices
	Event	evs[Nevs];	// events from CM11

	CM11	cm11;
};

int debug=0;

// housecode 'A'..'P
static char hcodes[] = {
	0x6, 0xe, 0x2, 0xa, 0x1, 0x9, 0x5, 0xd, 
	0x7, 0xf, 0x3, 0xb, 0x0, 0x8, 0x4, 0xc
};

// device code 1..16
static char dcodes[] = {
	0x6, 0xe, 0x2, 0xa, 0x1, 0x9, 0x5, 0xd, 
	0x7, 0xf, 0x3, 0xb, 0x0, 0x8, 0x4, 0xc
};

/* dcbit[dc] bit's on if dc is in mask
 */
static ushort dcbit[] = {
	0x4000, 0x0040, 0x0400, 0x0004,
	0x0200, 0x0002, 0x2000, 0x0020,
	0x8000, 0x0080, 0x0800, 0x0008,
	0x0100, 0x0001, 0x1000, 0x0010,
};
	
// function names
char* x10fnames[] = {
	"alloff",
	"lightson",
	"on",
	"off",
	"dim",
	"bright",
	"lightsoff",
	"ext",
	"hailreq",
	"hailack",
	"psdim1",
	"psdim2",
	"extxfer",
	"stson",
	"stsoff",
	"stsreq"
};


Dev*
x10devs(X10* x)
{
	return &x->devs[0];
}

uchar
hctochr(uchar c)
{
	int	 i;

	for (i = 0; i < nelem(hcodes); i++)
		if (c == hcodes[i])
			return 'a'+i;
	return '?';
}

uchar
chrtohc(uchar hc)
{
	hc = tolower(hc);
	if (hc < 'a')
		hc = 'a';
	if (hc > 'p')
		hc = 'p';
	hc -= 'a';
	return hcodes[hc];
}

uchar
dctoint(uchar c)
{
	int	 i;

	for (i = 0; i < nelem(hcodes); i++)
		if (c == hcodes[i]){
			return i+1;
		}
	return -1;
}

uchar
inttodc(uchar dc)
{
	if (dc < 1)
		dc = 1;
	if (dc > 16)
		dc = 16;
	dc -= 1;
	return dcodes[dc];
}

char*
fntostr(uchar f)
{
	static char unk[] = "Func?";

	if (f < nelem(x10fnames))
		return x10fnames[f];
	else
		return unk;
}

void
cm11sprint(X10* x, char* buf, int len)
{
	CM11*	c;
	char*	b;

	c = &x->cm11;
	b = seprint(buf, buf+len, "timer %04x ", c->timer);
	b = seprint(b, buf+len, "%d:%d:%d ", c->h, c->m, c->s);
	b = seprint(b, buf+len, "day %d days %x ", c->yday, c->wdays);
	b = seprint(b, buf+len, "hc %c vers %d ", hctochr(c->hc), c->vers);
	b = seprint(b, buf+len, "devs %04x sts %04x ", c->devs, c->sts);
	seprint(b, buf+len, "dim %04x\n", c->dim);
}

void
x10print(int fd, X10* x)
{
	static char*	ons[] = { "off", "on" };
	int	i;
	Addr*	a;
	Func*	f;
	Dev*	d;
	CM11*	c;

	fprint(fd, "dev at %s (hc %c):\n", x->dev, hctochr(x->hc));
	c = &x->cm11;
	fprint(2, "\tcm11: timer %04x ", c->timer);
	fprint(2, "%d:%d:%d ", c->h, c->m, c->s);
	fprint(2, "day %d days %x ", c->yday, c->wdays);
	fprint(2, "hc %c vers %d ", hctochr(c->hc), c->vers);
	fprint(2, "devs %04x sts %04x ", c->devs, c->sts);
	fprint(2, "dim %04x\n", c->dim);
	fprint(fd, "\n\tdevices: ");
	for (i = 0; i < Ndevs; i++){
		d = &x->devs[i];
		if (d->hc != 0){
			fprint(fd, "%c:%d=", hctochr(d->hc), dctoint(d->dc));
			fprint(fd, "[%s,%x] ", ons[d->on], d->dim);
		}
	}
	fprint(fd, "\n\tevents: ");
	for (i = 0; i < Nevs; i++){
		switch(x->evs[i].type){
		case Eaddr:
			a = &x->evs[i].a;
			fprint(fd, "%c:%d ", hctochr(a->hc), dctoint(a->dc));
			break;
		case Efunc:
			f = &x->evs[i].f;
			fprint(fd, "%c:%s", hctochr(f->hc), fntostr(f->fn));
			if (f->fn == Fext)
				fprint(fd, "[%x %x] ", f->data, f->cmd);
			else
				fprint(fd, "[%x]", f->val);
			break;
		}
	}
	fprint(fd, "\n");
}

static void
updatedevs(X10* x)
{
	int	i;
	int	di;
	Addr*	a;
	Func*	f;

	// Look at events reported
	// learn any address mentioned in the same hc
	// and update its on/off status by looking at funcions
	di = -1;
	for (i = 0; i < Nevs; i++){
		switch(x->evs[i].type){
		case Eaddr:
			a = &x->evs[i].a;
			if (a->hc != x->hc){
				di = -1;
				continue;
			}
			di = dctoint(a->dc)-1;
			x->devs[di].hc = x->hc;
			x->devs[di].dc = a->dc;
			break;
		case Efunc:
			if (di < 0)
				continue;
			f= &x->evs[i].f;
			if (f->hc != x->hc)
				continue;
			if (f->fn == Fon)
				x->devs[di].on = 1;
			if (f->fn == Foff)
				x->devs[di].on = 0;
			break;
		}
	}
	memset(x->evs, 0, sizeof(x->evs));

	// Add any other shown in cm11 devs & sts masks
	for (i = 0; i < Ndevs; i++){
		x->devs[i].on = 0;
		x->devs[i].dim = 0;
		x->devs[i].dc = inttodc(i+1);
		if (x->cm11.devs & dcbit[i])
			x->devs[i].hc = x->hc;
		if (x->cm11.sts & dcbit[i]){
			x->devs[i].hc = x->hc;
			x->devs[i].on = 1;
		}
	}
}

/*
 * X10 Protocol
 *	PC <->	CM11 interface
 *	req→
 *		←sum
 *	ack→
 *		←rtr
 *
 * The interface can also request Ipoll, Itmr at any time.
 */

static int
x10ringing(X10* x)
{
	int	fd;
	char	buf[80];

	fd = open(x->sdev, OREAD);
	if (fd < 0)
		return -1;
	memset(buf, 0, sizeof(buf));
	read(fd, buf, sizeof(buf));
	close(fd);
	buf[sizeof(buf)-1] = 0;
	return (strstr(buf, "ring") != nil);
}

// service an interface poll request (Ipoll)
static void
x10poll(X10* x)
{
	uchar	repl;
	uchar	l;
	uchar	buf[10];
	uchar*	bp;
	int	i, n;
	Func*	f;
	Addr*	a;
	int	max;

	max = 0;

	repl = Ppoll;
	memset(buf, 0, sizeof(buf));
	memset(x->evs, 0, sizeof(x->evs));
	if (debug)
		fprint(2, "poll ");
	do {
		write(x->fd, &repl, 1);
		i = read(x->fd, buf, 1);
	} while (i == 1 && buf[0] == Ipoll && max++ < 255);
	if (max >= 255){
		if (debug)
			fprint(2, "loop\n");
		syslog(0, logf, "x10 poll loop broken");
		return;
	}
	//write(x->fd, &repl, 1);
	if (i != 1){
		if (debug)
			fprint(2, "len read err: got %d bytes\n", i);
		syslog(0, logf, "len read err: got %d bytes\n", i);
		return;
	}
	l = buf[0];
	if (l > 9){
		syslog(0, logf, "big poll buffer %d (ignored)", l);
		if (debug)
			fprint(2, "big poll buffer %d\n", l);
		return;
	}
	if (debug)
		fprint(2, "%d bytes [", l);
	repl = Pack;
	for (i = 0; i < l; i++){
		read(x->fd, buf+1+i, 1);
		if (debug)
			fprint(2, "%02x ", buf[1+i]);
		write(x->fd, &repl, 1);
	}
	if (debug)
		fprint(2, "]\n");
	write(x->fd, &repl, 1);
	if (l > 0){
		l--; // don't count mask
		bp = buf+2;
		n = 0;
		for (i = 0; i < l; i++){
			if (buf[1]&(1<<n)){
				x->evs[n].type = Efunc;
				f = &x->evs[n].f;
				f->hc = (bp[i]>>4);
				f->fn = (bp[i]&0xf);
				if (f->fn != Fext)
					f->val = bp[++i];
				else {
					f->data= bp[++i];
					f->cmd = bp[++i];
				}
			} else {
				x->evs[n].type = Eaddr;
				a = &x->evs[n].a;
				a->hc = (bp[i]>>4);
				a->dc = (bp[i]&0xf);
			}
			n++;
		}
	}
	if (debug)
		x10print(2, x);
	updatedevs(x);
}

static void
ring(X10* x, int on)
{
	uchar	cmd;
	uchar	rep;

	if (on)
		cmd = Pringe;
	else
		cmd = Pringd;
	write(x->fd, &cmd, 1);
	read(x->fd, &rep, 1);
	write(x->fd, &cmd, 1);
	read(x->fd, &rep, 1);
}

// service a time request (Itmr)
// this also disables the ring signal.
static void
x10time(X10* x)
{
	uchar	req[7];
	Tm*	tm;

	if (debug)
		fprint(2, "time req ");
	syslog(0, logf, "power failed or controller reset");
	tm = localtime(time(nil));

	USED(tm);
	req[0] = Ptmr;
#ifdef notdef
	req[1] = tm->sec;
	req[2] = (tm->hour%2)*60 + tm->min;
	req[3] = tm->hour/2;
	req[4] = tm->yday & 0x0ff;
	req[5] = ((tm->yday & 0x100)>>1)|
		 (1 << (6-tm->wday));
#endif
	req[1] = 1;
	req[2] = 1;
	req[3] = 1;
	req[4] = 1;
	req[5] = 1;
	req[6] = ((x->hc << 4)|1);

	if (write(x->fd, req, sizeof(req)) != sizeof(req))
		syslog(0, logf, "can't write time\n");
	req[0] = -1;
	read(x->fd, req, 1);
	if (debug)
		fprint(2, "%x\n", req[0]);
	ring(x, 1);
}

static uchar
mksum(uchar* msg, int l)
{
	uint	s;
	int	i;

	s = 0;
	for (i = 0; i < l; i++){
		s+=msg[i];
		s&=0xff;
	}
	return s;
}

int
x10req(X10* x, Msg* m)
{
	int	i;
	uchar	msg[5];
	uchar	ack;
	uchar	buf[20];
	uchar	sum;
	uchar	msum;
	int	l;
	int	r;

	msg[0] = m->hdr;
	if (m->hdr != Xhdr){
		l = 2;
		msg[1] = m->code;
	} else {
		l = 5;
		msg[1] = m->func;
		msg[2] = m->unit;
		msg[3] = m->data;
		msg[4] = m->cmd;
	}
	msum = mksum(msg, l);
	if (x->fd < 0){
		syslog(0, logf, "x10req: no dev\n");
		return -1;
	}
	for(i = 0; i < 10; i++){
		if (debug)
			fprint(2, "msg [%ux %ux] sum %ux:",msg[0], msg[1], msum);
		if (write(x->fd, msg, l) != l){
			if (debug)
				fprint(2, "write: %r\n");
			syslog(0, logf, "x10req: write: %r\n");
			continue;
		}
		sum = 0;
		if (read(x->fd, &sum, 1) < 1){
			if (debug)
				fprint(2, "read: %r\n");
			syslog(0, logf, "x10req: read: %r\n");
			return -1;
		}
		if (sum != msum && sum == Ipoll){
			if (debug)
				fprint(2, "poll!\n");
			x10poll(x);
			continue;
		}
		if (sum != msum && sum == Itmr){
			if (debug)
				fprint(2, "time!\n");
			x10time(x);
			continue;
		}
		if (sum != msum){
			if (debug)
				fprint(2, "bad checksum (%x %x); retrying\n", sum, msum);
			continue;
		}
		if (debug)
			fprint(2, "sent\n");
		ack = Pack;
		for(r = 0; r < 5; r++){
			write(x->fd, &ack, 1);
			l = read(x->fd, buf, sizeof(buf));
			if (l == 1 && buf[0] == Irtr)
				return 0;
			else if (l == 1 && buf[0] == Ipoll){
				if (debug)
					fprint(2, "poll on rtr!\n");
				x10poll(x);
				return 0;
			} else if (l == 1 && buf[0] == Itmr){
				if (debug)
					fprint(2, "poll on rtr!\n");
				x10time(x);
				return 0;
			} else
				fprint(2, "req: not ready (%x); await\n", buf[0]);
			sleep(100);
		}
	}
	syslog(0, logf, "x10req: command failed\n");
	return -1;
}

/*
 * PC requests
 */

int
x10reqaddr(X10* x, char hc, char dc)
{
	Msg	m;

	x->hc = chrtohc(hc);
	dc = inttodc(dc);

	m.hdr = Hsync|Haddr|Hstd;
	m.code = ((x->hc&0xf)<<4) | (dc&0xf);
	return x10req(x, &m);
}

int
x10reqfunc(X10* x, int fn, int dim)
{
	Msg	m;

	if (dim < 0)
		dim = 0;
	if (dim > 100)
		dim = 100;
	dim = dim*Dimmax/100;
	m.hdr = (dim<<3)|Hsync|Hfunc|Hstd;
	m.code = (x->hc<<4)|(fn&0xf);
	return x10req(x, &m);
}

int
x10reqsts(X10* x)
{
	uchar	r;
	int	i;
	uchar	buf[14];
	uchar*	bp;
	int	nest;

	nest = 0;
again:
	nest++;
	r = Psts;
	write(x->fd, &r, 1);
	if (debug)
		fprint(2, "sts #%d [", nest);
	bp = buf;
	for (i = 0; i < 14; i++){
		r = Pack;
		read(x->fd, bp, 1);
		write(x->fd, &r, 1);
		if (debug)
			fprint(2, " %x", *bp);
		if (buf[0] == Ipoll){
			if (debug){
				if (x10ringing(x))
					fprint(2, "*ring*");
				fprint(2, "!poll]\n");
			}
			x10poll(x);
			goto again;
		}
		if (buf[0] == Itmr){
			if (debug)
				fprint(2, "!time]\n");
			x10time(x);
			goto again;
		}
		bp++;
	}
	if (debug)
		fprint(2, "]\n");
	x->cm11.timer = (buf[0]<<8)|buf[1];
	x->cm11.h = buf[4]*2+buf[3]/60;
	x->cm11.m = (buf[3]%60);
	x->cm11.s = buf[2];
	x->cm11.yday = buf[5]|((buf[6]&0x80)<<1);
	x->cm11.wdays= buf[6]&0x7f;
	x->cm11.vers = buf[7]&0xf;
	x->cm11.hc = buf[7]>>4;
	x->cm11.devs = (buf[8]<<8)|buf[9];
	x->cm11.sts  = (buf[10]<<8)|buf[11];
	x->cm11.dim = (buf[12]<<8)|buf[13];

	if (debug)
		x10print(2, x);
	updatedevs(x);
	return 0;
}

/*
 * CM11 eeprom:
 *	Macro offset		(2 bytes)
 *	Timer initiators		up to the first 0xff
 *	Macro initiators	up to macro offset.
 *	Macros
 *
 * It's downloaded in 3+Eblksz byte blocks, starting with
 * Peeprom and the eeprom address (3 + 16 bytes).
 *
 * This eeprom is meant to clear any macros
 * that come programmed from the factory or previous
 * use of the cm11
 */

static uchar eeprom[] = {
[0x00]	0x00,	// macro offset: 0x0003
[0x01]	0x0,	// 

[0x02]	0xff,	// End of timer initiators
};


static int
x10puteeblock(X10* x, ushort a, uchar* block)
{
	int	i;
	uchar	sum;
	uchar	isum;
	uchar	ack;

	if (debug)
		fprint(2, "eeprom block...\n");
	sum = 0;
	for (i = 0; i < Eblksz + 3; i++){
		if (debug){
			if (i >= 3)
				fprint(2, "[%02x]", a+i-3);
			fprint(2, "\t%02x\n",block[i]);
		}
		write(x->fd, block+i, 1);
		if (i != 0)
			sum = (sum + block[i]) & 0xff;
	}
	isum = 0;
	read(x->fd, &isum, 1);
	if (isum != sum){
		fprint(2, "puteeblock: bad checksum %x %x\n", isum, sum);
		return -1;
	}
	ack = Pack;
	write(x->fd, &ack, 1);
	read(x->fd, &ack, 1);
	if (ack != Irtr){
		fprint(2, "puteeblock: not ready");
		return -1;
	}
	return 0;
}

static void
putshort(uchar* bp, ushort s)
{
	bp[0] = (s&0xff);
	bp[1] = (s>>8);
}

static void
x10puteeprom(X10* x, uchar *e, int l)
{
	static uchar	block[3+Eblksz];
	ushort	addr;
	int	bl;

	addr = 0x0000;
	while(l > 0){
		memset(block, 0, sizeof(block));
		block[0] = Peeprom;
		putshort(block+1, addr);
		bl = l;
		if (bl > Eblksz)
			bl = Eblksz;
		memmove(block+3, e, bl);
		e += bl;
		l -= bl;
		if (x10puteeblock(x, addr, block) < 0){
			syslog(0, logf, "eeprom clear failed\n");
			return;
		}
		addr += Eblksz;
	}
	syslog(0, logf, "eeprom cleared\n");
}


X10*
x10open(char* dev, char hc)
{
	int	fd;
	char*	cdev;
	char	ctlstr[] = "b4800 c0 d0 e0 l8 m0 pn r1 s1 i0 ier=3";
	X10*	x;
	int	i;

	x = malloc(sizeof(X10));
	memset(x, 0, sizeof(X10));
	x->hc = chrtohc(hc);
	x->dev = strdup(dev);
	x->sdev= smprint("%sstatus", dev);
	x->fd = -1;
	cdev = smprint("%sctl", dev);
	fd = open(cdev, OWRITE);
	if (fd >= 0){
		write(fd, ctlstr, strlen(ctlstr));
		close(fd);
	}
	free(cdev);	
	x->fd = open(dev, ORDWR);
	if (x->fd < 0){
		syslog(0, logf, "open %s: %r\n", dev);
		fprint(2, "open %s: %r\n", dev);
		sysfatal("nodev");
	}
	x10time(x);
	x10puteeprom(x, eeprom, sizeof(eeprom));
	sleep(1000);
	for (i = 0; i < 5; i++)
		if (x10reqsts(x) >= 0)
			break;
	ring(x, 1);
	return x;
}

void
x10close(X10* x)
{
	close(x->fd);
	free(x);
}



