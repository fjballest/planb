
typedef struct Edit	Edit;
typedef struct Cmd	Cmd;
typedef struct Xcmd	Xcmd;
typedef struct Evh	Evh;
typedef struct Sts	Sts;

enum {
	Sclean,	// file is loaded and clean
	Sdirty,	// file is dirty
	Stemp,	// temporary panel (for errors)

	// Edit fonts
	FR = 0,
	FT = 1,

	// Clean policy constants
	Dnrun	= 10,	// start cleanup when more than Dnrun are used.
	Dnwins	= 6,	// clean until we get less than Dnwins used.
	Dtime	= 15*60,// keep Sclean edits younger than Dtime secs.

	Tagmax	= 4*1024,
};

/* Status for each editing panel
 */
struct Sts {
	int	sts;	// Sclean, Sdirty, Stemp
	int	ss, se;	// selection
	int	mark;	// insertion point
	int	font;	// FR or FT
};

/* Editing panel
 */
struct Edit{
	Sts;
	char*	name;	// path of file or edit name.
	char*	dir;	// of file
	Qid	qid;	// of file
	long	atime;	// time() as of last use.
	char*	text;	// being sent to/from omero
	char*	lastev;
	int	pid;	// of associated user process
	Panel*	gcol;	// column containing...
	Panel*	gtag;	// tag line
	Panel*	gtext;	// text edition panel
	char*	gcolname;//name of directory for gcol
};

/* Event handler
 */
struct Evh {
	char*	name;
	void	(*f)(Edit*, Oev*);
};

/* Builtin command
 */
struct Cmd {
	char*	name;
	int	(*f)(Edit*, int, char*[], int);
};

/* External command
 */
struct Xcmd {
	long	when;		// time() when launched.
	int	pid;		// of the external process
	char*	name;		// for the file shown in the edit buffer
	char*	dir;		// starting cwd for process
	char*	cmd;		// cmd line(s)
	char*	ox;		// path to ox column
	int	infd[2];	// pipe for stdin
	int	outfd[2];	// pipe for stdout
	int	errfd;		// stderr. when -1, use outfd.
	char*	tag;		// tag line
	Channel*pidc;		// to learn its pid
};

// |sort -bd +1
Xcmd*		allocxcmd(char* path, char* dir, char* cmd, char* ox);
int		cdone(Edit* e, int argc, char* argv[], int force);
void		cleanedit(Edit* e, Dir* d);
char*		cleanpath(char* file, char* dir);
void		cmdinit(void);
void		deledit(Edit* e);
void		dumpedits(void);
void		editcmd(Panel* gtext, char* ecmd, char* path);
Edit*		editfile(char* path, int temp);
int		editrun(Panel* t, char* dir, char* arg, char* path);
void		externrunevent(char* path, char* ev, char* arg);
char*		filedir(char* file);
void		freexcmd(Xcmd* c);
int		getsts(Panel* gtext, Sts* e);
char*		gettagpath(Edit* e);
void		inittagcmds(void);
int		loadfile(Edit* e, char* file);
void		look(Edit* e, char* arg, char* path);
void		msgprint(Edit* e, char* fmt, ...);
void		musthaveedits(void);
Edit*		musthavemsgs(char* msgs);
void		plumbinit(void);
void		regerror(char* msg);
void		run(Edit* e, char* arg, int, char* path);
void		tagcmds(char* fn, char* buf, int max);
void		threadmain(int argc, char* argv[]);
void		updatetag(Edit* e, int keep);
void		updatetext(Edit* e);
void		wctl(char* path, char* ctl);
void		xcmd(char* path, char* dir, char* arg, char* in, char* out, char* ox);
void		xcmdproc(void* a);
int		mustplumb(char* fn);
#define dprint if(debug)print

extern int debug;
extern Edit**	edits;
extern int	nedits;
