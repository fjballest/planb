typedef struct Vol	Vol;	// volume instance
typedef struct Fs	Fs;	// volume file server
typedef struct Frpc	Frpc;	// 9p msg.
typedef struct Prop	Prop;	// Attributes/properties
typedef struct Cnstr	Cnstr;	// Constraint
typedef struct Expr	Expr;	// Constraint expressions
typedef struct Mvol	Mvol;	// mounted volume
typedef struct Fid	Fid;	// guess what

enum {
	Maxmsglen= IOHDRSZ + 8 * 1024,	// Max message size

	Biglatency= 10000,	// a very bad latency
	Badlatency= 200,	// latency(ms) bad enough for /bin

	Nthreads = 64,		// # of work threads

	Nattrs = 32,		// Max # of attrs per constraint.
	Ncnstrs= 32,		// Max # of constraints per expression.
	Nrpcs	= 128,		// Max # of allocated rpcs
	Nmuxrpcs= 128,		// Max # of concurrent rpcs per fs
	Fhashsz	= 512,		// size of fid hash

	Wstack	= 8*1024,	// Stack size for worker procs
	Stack	= 8*1024,	// Stack size for our procs.

	Tmout	= 2 ,		// grain size for timers (ms)
	Rpctmout= 20,		// when to consider dead a request (20sec)
	Trytmout = 60,		// when to retry a dead fs (1min)
	Disctick = 60,		// secs for discovery protocol interval

	Any = 0,
	Incr = 16,

};

struct Prop {
	int	var;
	int	val;
};

struct Cnstr {
	Prop	attrs[Nattrs];
	int	nattrs;
};

struct Expr {
	Cnstr*	cnstrs[Ncnstrs];
	int	ncnstrs;
};


struct Frpc {
	uchar	buf[Maxmsglen];	// buf may be changed by convM2S and debug,
	uchar	sbuf[Maxmsglen];// we use sbuf to convS2M for sending.
	char	err[128];	// error message.
	Dir*	d;	// Override dir entry for stat.
	Fid*	fid;	// use by the fcall. Mostly for ctlfs and nilfs
	Fid*	freefid;// to release at the end of the fcall.
	Fcall	f;	// 9p file call, using buf.
	Fcall	r;	// 9p reply for the call
	Frpc*	rep;	// buffering for the reply
	int	flushed;
	ulong	time;	// for the request
	Frpc*	next;	// in free list or mux list
	Channel*sc;	// where to notify the thread waiting to this
};

struct Mvol {
	Ref;			// how many fids using it. Including attach
	QLock;			// for vols and fids
	long	epoch;		// of last updatemvol
	int	isunion;	// true for volume unions.
	int	musthave;	// can't fail. Hold rpcs if no vol avail.
	int	notimeout;	// don't timeout RPCs other than walk on this.
	char*	spec;		// attach spec. for debugging
	char*	name;		// name for the volume mounted
	Expr	cnstr;		// constraint expression
	int	choice;		// which cnstr in expr are we using?
	Vol**	vols;		// volumes mounted by this mount point.
	int	nvols;
	Fid*	fids;		// using this mvol.

	int	debug;		// debug this mount point.
};

struct Vol {
	Ref;			// # of mvols mounting it
	long	epoch;		// tstamp for addition/death
	int	dead;		// volume not available
	int	disabled;	// volume disabled by user
	char*	cfname;		// config file where we discovered it.
	char*	host;		// servicing it.
	char*	addr;		// where to reach the server
	char*	spec;		// for the file tree
	Name*	sname;		// path in server to / of vol
	char*	name;		// for the volume
	char*	cnstr;		// constraint as supplied by user
	Cnstr	ccnstr;		// compiled constraints
	Fs*	fs;		// connection to server
	Fid*	slash;		// fid to / for vol.
};

struct Fs {
	Ref;		// # of vols relying on it.
	long	epoch;	// tstamp for addition/death
	int	fd;	// to server
	int	musthave;// at least one fid requested as *must* this fs.
	Qid	fdqid;	// qid for /srv file to server. For amountfs.
	long	time;	// when last op was started or declared dead
	char*	addr;	// where to reach the 9p server
	char*	raddr;	// addr once resolved by cs (cached)
	char*	spec;	// for its tree in attach
	Frpc*	rpcs;	// list of active rpcs
	Channel*reqc;	// of Freq, to send requests.
	Channel*repc;	// replies from the fs read proc sent here
	Channel*tmrc;	// periodic tick for this fs
	int	pid;	// of read proc
	Fid*	fid;	// for slash, while attached
	Qid	qid;	// used to decorate real qids for fids
	long	lat;	// ms needed to mount the fs.
};

struct Fid {
	QLock;
	Fid*	hnext;	// on hash or freelist
	Fid*	mnext;	// on Mvol
	int	linked;	// on Mvol through mnext
	int	epoch;	// for its fs when fid was bound to it.
	int	stale;	// true when fid goes to a gone volume in its mvol
	Mvol*	mvol;	// client's mount point where it resides
	Name*	sname;	// relative path for file in volume (server name)
	int	nr;	// fid number used by client (we hash on it)
	int	snr;	// fid number used to talk to the server
	Dir*	d;
	Qid	qid;	// first qid seen by client
	Fs*	fs;	// Nil or FS for this fid when snr is valid.

	int	notimeout;// true if should not tmout reqs other than walk
	int	isopen;	// true if fid is open
	int	omode;	// open mode when open
	int	iounit;
	char*	ureadbuf;// for reading unions and ctl files
	long	ureadlen;

	int	debug;	// true if fid is being debugged
};

typedef void (*Fscall)(Frpc*);

#pragma     varargck    type  "V"   Vol*
#pragma     varargck    type  "W"   Mvol*
#pragma     varargck    type  "X"   Fid*
#pragma     varargck    type  "K"   Expr*
#pragma     varargck    type  "k"   Cnstr*

Frpc*		rpcalloc(void);
Name*		n_new(void);
void		adsproc(void*);
void		dumpfids(void);
void		dumpfss(void);
void		dumpmvols(void);
void		dumpvols(void);
void		exprinit(void);
void		fsmuxthread(void*arg);
void		sendads(void);
Fid *		fidalloc(int nr);
int		postfd(char *name, int pfd);
int		noop(Fs* , Frpc* fop);
Fs*		getfs(char* addr, char* spec);
int		getfidnr(void);
void		threadmain(int argc, char *argv[]);
void		putfs(Fs* c);
void		cnstrcat(Cnstr* c1, Cnstr* c2);
void		n_cat(Name* cn, Name* n);
void		exprfree(Expr* e);
int		cnstrmatch(Cnstr* env, Cnstr* c);
int		exprmatch(Cnstr* env, Expr* exp);
int		Vfmt(Fmt* f);
int		Wfmt(Fmt* f);
int		namefmt(Fmt* f);
int		getfcall(int fd, Frpc* m);
int		getfrep(int fd, Frpc* m);
int		putfcall(int fd, Frpc* m);
int		putfrep(int fd, Frpc* m);
int		walkfid(Fid* fid, Fid* nfid, char** elems, int nelems, Frpc* fop);
int		fidfree(Fid* fid, Frpc* fop);
int		Kfmt(Fmt* fmt);
int		fidfmt(Fmt* fmt);
int		kfmt(Fmt* fmt);
void		config(char* fname);
int		checkvols(Frpc* fop);
void		rpcerr(Frpc* fop, char* err);
int		amountfs(Fs* fs);
int		fsop(Fs* fs, Frpc* op);
int		islocal(char* host);
Cnstr*		parsecnstr(char* ln, Cnstr* c);
Expr*		parseexpr(char* ln, Expr* e);
void		cmdline(char* ln, char* fname, int lno);
void		putmvol(Mvol* m, Frpc* fop);
void		rpcfree(Frpc* m);
void		mvoladdfid(Mvol* m, Fid* fid);
void		mvoldelfid(Mvol* m, Fid* fid);
void		updatemvol(Mvol* m, Frpc* fop);
Fid*		mvolgetfid(Mvol* m, Name* sname, Fid* excl);
void		mvolunmount(Mvol* m, Vol* v, Frpc* fop);
Vol*		getmvolvol(Mvol* m, int i);
void		n_dotdot(Name* n);
void		n_free(Name* n);
void		n_reset(Name* n);
void		n_append(Name* n, char* s);
void		n_setpos(Name* n, int a, int b);
void		n_getpos(Name* n, int* a, int* b);
int		n_eq(Name* n1, Name* n2);
void		fstimer(ulong now);
void*		erealloc(void* p, int sz);
char*		estrdup(char* s);
Mvol*		newmvol(char* spec);
void*		emalloc(int sz);
void		deadvol(Vol* v, Frpc* fop);
int		volmount(Vol* v, Frpc* fop);
void		closefid(Fid* f, Frpc* fop);
void		fsstats(void);
void		fdconfig(int,char*);
void		newfsqid(Qid*);
void		mvoldelotherfids(Mvol* m, Fid* fid);

#define dprint	if(debug)fprint
#define vdprint	if(vdebug)fprint
void	dbgprint (Fid*,char*,...);
void	Dbgprint (Fid*, char*,...);
void	tracemvol(char*,int);

#pragma     varargck    argpos  dbgprint 2
#pragma     varargck	argpos	Dbgprint 2


extern int	debug, vdebug, hdebug;

extern Fscall	fscalls[Tmax];
extern Vol**	vols;
extern int	nvols;
extern QLock	volslck;
extern Mvol**	mvols;
extern int	nmvols;
extern QLock	mvolslck;
extern Ref	epoch;
extern int	dontsendads;
extern int	msglen;
extern Fs	ctlfs;
extern int	(*(ctlfsops[]))(Fs*,Frpc*);
extern int	(*(nilfsops[]))(Fs*,Frpc*);
extern uvlong	Ctldirqid;
extern uvlong	Ctlqid;
extern Cnstr	netokcnstr, netbadcnstr;
extern char	Eopen[];
extern char	Enotopen[];
extern char	Eio[];
extern char	Eperm[];
extern char	Enotexist[];
extern char	Eauth[];
extern char	Euser[];
extern char	Espec[];
extern char	Edupfid[];
extern char	Ebadfid[];
