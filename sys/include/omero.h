#pragma	lib	"libomero.a"
#pragma src "/sys/src/libomero"

typedef struct Panel Panel;
typedef struct Oev Oev;
typedef struct Repl Repl;

enum {
	Npaths = 10,

	Fdata = 0,
	Fctl,

};

struct Repl{
	Repl*	next;	// in replica list
	char*	path;	// e.g., "/n/.../row:2/slider:volume"
	int	fd[2];	// data and ctl fds
};

struct Panel{
	QLock;		// name is WORM, so mostly for repl.
	Ref;		// One for user + one per event pointing to it.
	int	gone;	// true if the panel is gone.
	char*	name;	// e.g., "slider:volume"
	Panel*	next;	// in list of panels
	Repl*	repl;	// known replicas
	int	nrepl;
	Channel*evc;	// events here or through oeventchan(nil)
};

struct Oev {
	char*	path;	// to repl
	char*	ev;
	char*	arg;

	Panel*	panel;
};

/* User interface. because of
 * replication we have to provide many I/O functions, sic.
 */
Panel*		createpanel(char* name, char* type, char* omero);
Panel*		createsubpanel(Panel* g, char* name);
char*		panelpath(Panel*);
int		openpanel(Panel* g, int omode);
int		openpanelctl(Panel* g);
void		closepanel(Panel* g);
void		closepanelctl(Panel* g);
vlong		seekpanel(Panel*g, vlong pos, int type);
long		readpanel(Panel* g, void* buf, long len);
Dir*		dirstatpanel(Panel* g);
void*		readallpanel(Panel* g, long* l);
long		writepanel(Panel* g, void* buf, long len);
long		readpanelctl(Panel* g, void* buf, long len);
long		writepanelctl(Panel* g, void* buf, long len);
void		omeroterm(void);
Channel*	omeroeventchan(Panel* g);
void		clearoev(Oev* e);
void		removepanel(Panel* g);
int		panelctl(Panel* g, char* fmt, ...);

// Helpers
void		paneldump(int fd);
int		plumbexec(char* dir, char* arg);
int		plumblook(char* dir, char* arg);
void		evhistory(char* prg, char* ev, char* arg);
Channel*	createportproc(char* name);
int		createport(char* name);

// Implementation. Do not use
void		movepanel(char* from, char* to);
Repl*		findrepl(Panel* g, char* path, int mkit);
void		wpanelexcl(Panel* g, char* what, void* buf, long len, Repl* excl);
Panel*		findpanel(char* n, int mkit);
Panel*		mkpanel(char* path);
void		rpaneldata(Panel* g, Repl* r);
Panel*		newpanel(char* path, int mkit);

extern char*	appluiaddress;
extern int	omerodebug;
