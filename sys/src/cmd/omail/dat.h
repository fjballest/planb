typedef struct Exec Exec;
typedef struct Message Message;
typedef struct Window Window;

enum
{
	STACK		= 8192,
	EVENTSIZE	= 256,
	NEVENT		= 5,
};

struct Window
{
	char*	name;
	Panel*	g;
	Panel*	gtag;
	Panel*	gtext;
	Biobuf*	body;
	char*	bodyf;
};

struct Message
{
	Window	*w;
	int		ctlfd;
	char		*name;
	char		*replyname;
	uchar	opened;
	uchar	dirty;
	uchar	isreply;
	uchar	deleted;
	uchar	writebackdel;
	uchar	tagposted;
	uchar	recursed;
	uchar	level;
	uchar	saved;
	uchar	replied;
	/* header info */
	char		*fromcolon;	/* from header file; all rest are from info file */
	char		*from;
	char		*to;
	char		*cc;
	char		*replyto;
	char		*date;
	char		*subject;
	char		*type;
	char		*disposition;
	char		*filename;
	char		*digest;

	Message	*next;	/* next in this mailbox */
	Message	*prev;	/* prev in this mailbox */
	Message	*head;	/* first subpart */
	Message	*tail;		/* last subpart */
};

enum
{
	NARGS		= 100,
	NARGCHAR	= 8*1024,
	EXECSTACK 	= STACK+(NARGS+1)*sizeof(char*)+NARGCHAR
};

struct Exec
{
	char		*prog;
	char		**argv;
	int		p[2];	/* p[1] is write to program; p[0] set to prog fd 0*/
	int		q[2];	/* q[0] is read from program; q[1] set to prog fd 1 */
	Channel	*sync;
};

extern	Window*	newwindow(char* name);
extern	void		winopenbody(Window*);
extern	void		winclosebody(Window*);
extern	void		wintagwrite(Window*, char*, int);
extern	int		windel(Window*);
extern	void		winwritebody(Window*, char*, int);
extern	void		winclean(Window*);
extern	char*	winreadbody(Window*, int*);
extern	void		readmbox(Message*, char*, char*);
extern	void		rewritembox(Window*, Message*);

extern	void		mkreply(Message*, char*, char*, char*, Plumbattr*, char*);
extern	void		delreply(Message*);

extern	int		mesgadd(Message*, char*, Dir*, char*);
extern	void		mesgmenu(Window*, Message*);
extern	int		mesgopen(Message*, char*, char*, Message*, char*);
extern	void		mesgctl(void*);
extern	void		mesgsend(Message*);
extern	void		mesgdel(Message*, Message*);
extern	void		mesgmenudel(Window*, Message*, Message*);
extern	void		mesgmenumark(Window*, char*, char*);
extern	void		mesgmenumarkdel(Window*, Message*, Message*, int);
extern	Message*	mesglookup(Message*, char*, char*);
extern	Message*	mesglookupfile(Message*, char*, char*);
extern	void		mesgfreeparts(Message*);

extern	char*	readfile(char*, char*, int*);
extern	char*	readbody(char*, char*, int*);
extern	void		ctlprint(int, char*, ...);
extern	void*	emalloc(uint);
extern	void*	erealloc(void*, uint);
extern	char*	estrdup(char*);
extern	char*	estrstrdup(char*, char*);
extern	char*	egrow(char*, char*, char*);
extern	char*	eappend(char*, char*, char*);
extern	void		error(char*, ...);
extern	int		tokenizec(char*, char**, int, char*);
extern	void		execproc(void*);

#pragma	varargck	argpos	error	1
#pragma	varargck	argpos	ctlprint	2

extern	Window	*wbox;
extern	Message	mbox;
extern	Message	replies;
extern	char		*fsname;
extern	int		plumbsendfd;
extern	int		plumbseemailfd;
extern	char		*home;
extern	char		*outgoing;
extern	char		*mailboxdir;
extern	char		*user;
extern	char		deleted[];
extern	int		wctlfd;
extern	int		shortmenu;
extern	int		debug;

#define dprint	if(debug)fprint
