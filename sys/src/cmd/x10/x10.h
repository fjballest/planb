
typedef struct Msg Msg;
typedef struct X10 X10;
typedef struct Addr Addr;	// device address
typedef struct Dev Dev;		// device


/*
 * X10 messages
 */
enum {

	Dimmax	= 22,	// 100% dims
	Ndevs	= 16,

	// Msg.hdr:	dim:Hsync:Hfunc|Haddr:Hext|Hstd
	Hsync	= 0x4,
	Hfunc	= 0x2,
	Haddr	= 0x0,
	Hext	= 0x1,
	Hstd	= 0x0,

	// Extended msg header
	Xhdr	= Hsync|Hfunc|Hext,

	// Msg.code:	<hcode>:<dcode>|<fn>

	// X10 Functions
	Falloff		= 0x0,
	Flightson	= 0x1,
	Fon		= 0x2,
	Foff		= 0x3,

	Fdim		= 0x4,
	Fbright		= 0x5,
	Flightsoff	= 0x6,
	Fext		= 0x7,

	Fhailreq	= 0x8,
	Fhailack	= 0x9,
	Fpsdim1		= 0xa,
	Fpsdim2		= 0xb,

	Fextxfer	= 0xc,
	Fstson		= 0xd,
	Fstsoff		= 0xe,
	Fstsreq		= 0xf,
	Fmax,
};

struct Msg{
	uchar	hdr;
	union {
		// regular message
		struct {
			uchar	code;
		};
		// extended message
		struct {
			uchar	func;
			uchar	unit;
			uchar	data;
			uchar	cmd;
		};
	};
};

enum {
	// PC requests/replies
	Pack	= 0x00,	// sum ok
	Ppoll	= 0xc3,	// Ok to upload
	Ptmr	= 0x9b,	// Timer download
	Psts	= 0x8b,	// CM11 sts request
	Peeprom	= 0xfb,	// starting to download eeprom
	Pringe	= 0xeb,	// enable the ring
	Pringd	= 0xdb,	// disable the ring

	// Interface requests/replies
	Irtr	= 0x55,	// ready to receive
	Itmr	= 0xa5,	// set timer (power fail)
	Ipoll	= 0x5a,	// poll to PC (upload events)

	// eeprom
	Eblksz	= 16,	// download block data size
};

/*
 * X10 user interface
 */

struct Addr {
	uchar	hc;	// house code
	uchar	dc;	// device code, if != 0
};

struct Dev {
	Addr;
	int	on;
	int	dim;
};

extern char*	x10fnames[];
extern int	debug;
extern int	interactive;
extern char	logf[];

X10*		x10open(char* dev, char hc);
void		x10close(X10* x);
void		x10print(int fd, X10* x);
int		x10req(X10* x, Msg* m);
int		x10reqaddr(X10* x, char hc, char dc);
int		x10reqfunc(X10* x, int fn, int dim);
int		x10reqsts(X10* x);
Dev*		x10devs(X10* x);

uchar		dctoint(uchar c);
uchar		hctochr(uchar c);
uchar		inttodc(uchar dc);
char*		fntostr(uchar f);
uchar		chrtohc(uchar hc);

/*
 * file system
 */
void		fs(X10* p, char hc, char* conf, char* srv, char* mnt);
void		pfs(X10* p, char hc, char* conf);
int		runfunc(X10* x, int nargs, char** args);
void		cm11sprint(X10* x, char* buf, int len);
