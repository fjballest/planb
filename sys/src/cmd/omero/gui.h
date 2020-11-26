typedef struct Panel	Panel;
typedef struct Pops	Pops;
typedef struct Plabel	Plabel;
typedef struct Pimage	Pimage;
typedef struct Pgauge	Pgauge;
typedef struct Pframe	Pframe;
typedef struct Pdraw	Pdraw;
typedef struct Con	Con;
typedef struct Edit	Edit;
typedef struct Pcolrow	Pcolrow;
typedef struct Tblock	Tblock;
typedef struct Cmouse	Cmouse;

/*
 * Locking and misc conventions:
 *
 * - A File holds its Panel in f->aux, which may be nil
 * 	while the panel is being created.
 * - p->dfile/cfile/file may be nil while being deleted.
 * - All of dir/ctl/data hold refs to the panel, and
 * 	the panel is removed once the last ref is gone.
 * - No locking is done while creating the panel.
 * - Rlocks on data file protect readers of the panel.
 * - Wlocks on data file protect writers of the panel.
 * - Only lib9p locks multiple files at the same time.
 */

enum {
	// Grouping panel types;
	Qnone,	// no panel, used to catch up bugs
	Qrow,	// Lay out in columns
	Qcol,	// Lay out in rows

	// Sizes
	Inset	= 3,	// empty space for inner elements
	Tagwid	= 10,	// size of the window tag box
	Taght	= 10,

	// Window events through Panel.wevc
	// when processed, send anything through Panel.wevr
	Winit = 0,
	Wexit = 0,

	// Panel flags
	Phide	= 0x001,	// panel is not shown
	Ptop	= 0x002,	// A top-level layout container
	Playout	= 0x004,	// A layout container, should reclaim space always.
	Pedit	= 0x008,	// text can be edited with mouse/kbd
	Ptag	= 0x010,	// must show the tag, no matter the default rule.
	Pmore	= 0x020,	// has more inner ones that are Phide
	Pline	= 0x040,	// (text panel) use at most one line.
	Pdirty	= 0x080,	// has unsaved changes, for tag.
	Phold	= 0x100,	// ignore show requests under it. (hold changes).
	Predraw	= 0x200,	// panel must be drawn (by showtree).
	Pdead	= 0x400,	// Some of the panel file's were rm'd. 

	// Fonts
	FR = 0,	// regular	(Vera 12pt)
	FB,	// bold		(Vera bold 12pt)
	FT,	// teletype	(Vera mono 12pt)
	FL,	// large	(Vera mono 20pt)
	FS,	// small		(Vera mono 10pt)
	NFONT,

	// Extra colors besides NCOL...
	HBORD = NCOL,
	BORD2,
	WS1,
	WS2,
	WS3,
	MAXCOL,

	// Colors
	CBack	= 0xFAFAFFFF,	// VERY light grey
	CHigh	= 0xFFAA00FF,
	CSet	= 0x5555AAFF,	// blue
	CClear	= 0xADADADFF,	// grey

	// Images for borders and tags
	Bback	= 0,	// background (no border)
	Btag	= 1,	// tag image
	Bdtag	= 2,	// dirty tag
	Bmtag	= 3,	// maximized tag
	Bdmtag	= 4,	// maximized dirty tag
	Bn	= 5,	// north border
	Bs	= 6,	// south border
	Be	= 7,	// east border
	Bw	= 8,	// west border
	Bws1	= 9,	// workspace tag #1 (#0 has no tag)
	Bws2	= 10,	// workspace tag #2
	Bws3	= 11,
	Bnw,
	Bne,
	Bse,
	Bsw,
	NBORD,

	// addedit op's
	Ins = 0,
	Del,
};

struct Plabel {
	char*	text;
};

struct Pimage {
	Image*	image;
	Rectangle irect;
	uchar*	idata;	// image raw data
	int	isize;	// length of idata
	int	voff;	// vertical offset for page
	int	hoff;	// horizontal offset for page
};

struct Pdraw {
	Rectangle drect;// image rectangle
	Image*	canvas;	// where draws work
	char*	dcmds;	// image drawing commands.
	int	dsize;
	int	dloaded;// true if has a draw to show.
};

struct Pgauge {
	int	pcent;
	Rectangle grect;
};


struct Edit {
	short	op;	// true for insert
	short	end;	// true if it completes an edit.
	Rune*	r;	// runes inserted; nil for deletion
	int	pos;	// position where inserted/deleted
	int	nr;	// number of runes
};

struct Pframe {
	Tblock*	blks;

	char	partial[UTFmax];	// partial rune sent from user
	int	npartial;
	int	partialoff;

	Frame	f;
	Image*	fimage;
	int	froff;	// offset in text for rune #0 in frame

	int	ss, se;	// start/end selection offsets in text
	int	s0;	// offset in text for 1st selection point
	int	sdir;	// direction of selection: >0 right; < 0 left
	int	mpos;	// last position set by mouse
	Rune*	snarf;
	int	nsnarf;

	Edit*	undos;	// edit operations that can be undone
	int	nundos;	// # such ops
	Edit*	undo;	// where to place next edit op in the list.
	int	cundo;	// index in undos for a clean frame.

	int	nlines;	// # lines in text (to compute maxsz)
	int	ncols;	// used to report size
	int	nrows;	// idem
	int	mark;	// floating position mark kept for users
};

struct Pcolrow {
	uint	nwins;	// # of inner winsows shown || 0 for all
};

struct Con {
	Ref;
	char*	addr;	// netaddr of event receiver
	int	fd;	// connection to it.
};

struct Pops {
	char*		pref;
	void		(*init)(Panel*);
	void		(*term)(Panel*);
	int		(*ctl)(Panel*, char*);
	long		(*attrs)(Panel*,char*,long);
	long		(*read)(Panel*,void*,long,vlong);
	long		(*write)(Panel*,void*,long,vlong);
	void		(*draw)(Panel*, int);
	void		(*mouse)(Panel*, Cmouse* m, Channel* mc);
	void		(*keyboard)(Panel*, Rune);
	long		(*writeall)(Panel*, void*, long);
	void		(*truncate)(Panel*);
};

struct Panel {
	Ref;		// one per file holding it in its aux field.

	Panel*		link;	// to next allocated panel
	int		type;	// assigned automatically
	char*		name;	// suffix of the file name; don't touch.
	char*		path;	// cached full path for its file
	File*		file;	// its correspondant directory.
	File*		cfile;	// its ctl file
	File*		dfile;	// its data file
	Con*		con;	// where to send its events
	long		atime; // time() as of last kbd/mouse event
	long		ctllen;// ctl file length, if not default

	int		flags;
	int		space;	// id of workspace; 0 for boot ws.
	int		holdfid;// fid that sets Phold (to clear on clunk)
	Point		wants;	// willing to get more x room
	Point		minsz;	// Dx/Dy that we'd like as a minimum.
	Point		maxsz;	// Dx/Dy that we would use at most, or ZP.
	Font*		font;

	Rectangle	rect;	// where shown.
	Rectangle	orect;	// where shown before last layout()
	int		ochild;	// old #sons. to force redraw if changed.
	Point		size;	// dx/dy wanted. Can be != Dx/Dy of rect.

	char*		writebuf;// for atomic writes to all the panel.
	long		nawrite; // # of bytes allocated in writebuf
	long		nwrite;	// # of bytes used in writebuf

	struct {
		Pcolrow;
		Plabel;
		Pimage;
		Pdraw;
		Pgauge;
		Pframe;
	};
};

enum {
	Nblock= 64,	// exercise blocks
	//Nblock= 8*1024,
};

struct Tblock {
	Tblock*	next;
	Tblock*	block0;
	int	nr;
	int	ar;
	Rune*	r;
};

#pragma varargck	type "T"	Tblock*

// |sort -bd +1
int		Mfmt(Fmt* f);
int		Tfmt(Fmt* f);
Edit*		addedit(Panel* p, int op, Rune* r, int nr, int pos);
void		addframesel(Panel* p, int pos);
void		argcursor(int yes);
char*		block2str(Tblock* bl, long*l);
void		blockcat(Tblock* b, Tblock* nb);
void		blockdel(Tblock* b, int n, int len, Rune* rp);
void		blockdump(Tblock* b);
void		blockfree(Tblock* b);
void		blockget(Tblock* b, int n, int len, Rune* rp);
void		blockins(Tblock* b, int off, Rune* r, int len);
long		blocklen(Tblock* b);
Tblock*		blockseek(Tblock* b, int *np, int off);
long		blocksize(Tblock* b);
void		borders(File* f);
void		cleanedits(Panel* p);
void		cleanpolicy(File* f);
void		closecon(Con* c);
void		closepanel(Panel* p);
int		conprint(Con* c, char* fmt, ...);
int		cookclick(Cmouse* m, Channel* mc);
Channel*		cookmouse(Channel* mc);
int		cookslide(Cmouse* m, Channel* mc);
void		detachfile(File* f);
void		drawtag(Panel* p, int setdirty);
void		event(Panel* p, char* fmt, ...);
char*		filepath(File* f);
void		fillframe(Panel* p);
int		findln(Tblock* b, int* pp);
int		findrln(Tblock* b, int* pp);
int		flagothersons(File* f, int set, int clr, File* excl);
int		flagsons(File* f, int set, int clr, int first, int last);
int		fontheight(Font* f);
int		framedel(Panel* p, Rune* r, int nr, int pos);
void		framedump(Panel* p);
Rune* 		framegetword(Panel* p, int pos, int* ss, int* se);
int		framehassel(Panel* p);
int		frameins(Panel* p, Rune* r, int nr, int pos);
void		fullwin(File* f);
int		genctl(Panel* p, char* ctl);
long		sprintattrs(Panel* p, char* str, long l, char* attr);
long		genattrs(Panel* p, char* str, long l);
int		genmouse(Panel* p, Cmouse* m, Channel* mc);
int		gettextht(Panel* p);
int		gettextwid(Panel* p);
int		hasedits(Panel* p);
int		hassons(File* f, int flag);
int		hastag(File*f);
int		hidden(File* f);
void		insertfile(File* from, File* dir, File* after);
int		intag(Panel* p, Point xy);
void		jumpframesel(Panel* p);
void		jumpframepos(Panel* p, int pos);
void		layout(File* f);
void		minwin(File* f);
int		mousecmdarg(File* f, Cmouse* m, Channel* mc);
void		rmhuppanels(File* f, Con* c);
Tblock*		newblock(Rune* r, int nr);
Con*		newcon(char* addr);
void		newedit(Panel* p);
File*		newfspanel(File* d, char* name, char* uid);
Panel*		newpanel(char* name, Panel* parent);
long		off2pos(Tblock* b, long off);
void		packblock(Tblock* b);
int		panelctl(Panel* p, char* op, int* mustdraw);
File*		pointinpanel(Point xy, int atomok);
long		pos2off(Tblock* b, long pos);
int		posselcmp(Panel* p, int pos);
void		pstring(Point pt, Image* color, Font* font, char* s, Rectangle clipr);
void		putscreen(void);
int		redo(Panel* p);
void		reloadframe(Panel* p, int resized);
void		resize(void);
void		rmpanel(File* f);
int		runeslen(Rune* r);
int		scrollframe(Panel* p, int nscroll);
void		setctlfilelen(Panel* p);
void		setframefont(Panel* p, Font* f);
void		setframemark(Panel* p, int pos);
void		setframesel(Panel* p, int ss, int se, int mpos);
int		setframesizes(Panel* p);
void		setnoedits(Panel* p);
void		shiftsonsleft(File* f);
void		shiftsonsright(File* f);
void		showtree(File* f, int force);
void		startproc(void*a);
Tblock*		str2block(char* s);
void		tagmousecmd(File *f, Cmouse* m, Channel* mc);
void		textdel(Panel* p, Rune* r, int nr, int pos);
int		textins(Panel* p, Rune* r, int nr, int pos);
void		threadmain(int argc, char **argv);
void		initui(void);
int		undo(Panel* p);
void		usage(void);
Rune*		utf2runes(char* s, int* lp, int* nlines);
int		wcommand(char* c);
int		within(File* d, File* f);
void		maxwin(File* f);
File*		paneltop(File* f);
void		incwin(File* f);
long		genreadbuf(void* data, long count, vlong off, void *s, long n);
void		genkeyboard(Panel* p, Rune r);
void		panelok(Panel*);
void		setterm(char*);
File**	newfilewalk(File*);
void	endfilewalk(File**);

#define	ispanel(f)	((f) && ((f)->qid.type&QTDIR) && (f)->aux)

#define Ox	"/bin/ox"

extern File*	slash;
extern Image*	cols[MAXCOL];
extern Image*	bord[NBORD];
extern Font*	fonts[NFONT];
extern Channel*	panelhupc;
extern char*	sname;
extern char*	saddr;
extern Channel*	startc;
extern Channel* destroyc;

#define dprint if(eventdebug)print
#define ldprint if(layoutdebug)print
#define fdprint if(framedebug)print
#define edprint if(eventdebug)print
#define Edprint if(eventdebug>1)print
#define tdprint if(textdebug)print
#define cdprint if(condebug)print
#define bdprint if(blockdebug)print
extern int	blockdebug;
extern int	layoutdebug;
extern int	framedebug;
extern int	eventdebug;
extern int 	textdebug;
extern int 	condebug;
extern int 	smallomero;
extern Pops*	panels[];
extern Point	lastxy;