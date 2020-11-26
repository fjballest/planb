typedef struct Message Message;
typedef struct Mailbox Mailbox;
typedef struct Header Header;
typedef struct Inbuf Inbuf;


enum {
	Buffersize = 64*1024,
};

enum
{
	// encodings
	Enone=	0,
	Ebase64,
	Equoted,

	// disposition possibilities
	Dnone=	0,
	Dinline,
	Dfile,
	Dignore,

	PAD64=	'=',
};

struct Mailbox
{
	QLock;
	int	refs;
	Mailbox	*next;
	int	id;
	int	dolock;		// lock when syncing?
	int	std;
	char	name[Elemlen];
	char	path[Pathlen];
	Dir	*d;
	Message	*root;
	int	vers;		// goes up each time mailbox is read

	ulong waketime;
	void	*aux;		// private to Mailbox implementation
};

struct Message
{
	int	id;
	int	refs;
	int	subname;
	char	name[Elemlen];

	// pointers into message
	char	*start;		// start of message
	char	*end;		// end of message
	char	*header;	// start of header
	char	*hend;		// end of header
	int	hlen;		// length of header minus ignored fields
	char	*mheader;	// start of mime header
	char	*mhend;		// end of mime header
	char	*body;		// start of body
	char	*bend;		// end of body
	char	*rbody;		// raw (unprocessed) body
	char	*rbend;		// end of raw (unprocessed) body
	char	*lim;
	char	deleted;
	char	inmbox;
	char	mallocd;	// message is malloc'd
	char	ballocd;	// body is malloc'd
	char	hallocd;	// header is malloce'd

	// mail info
	String	*unixheader;
	String	*unixfrom;
	String	*unixdate;
	String	*from822;
	String	*sender822;
	String	*to822;
	String	*bcc822;
	String	*cc822;
	String	*replyto822;
	String	*date822;
	String	*inreplyto822;
	String	*subject822;
	String	*messageid822;
	String	*addrs;
	String	*mimeversion;
	String	*sdigest;

	// mime info
	String	*boundary;
	String	*type;
	int	encoding;
	int	disposition;
	String	*charset;
	String	*filename;
	int	converted;
	int	decoded;
	char	lines[10];	// number of lines in rawbody

	Message	*next;		// same level
	Message	*part;		// down a level
	Message	*whole;		// up a level

	uchar	digest[SHA1dlen];

	vlong	imapuid;	// used by imap4

	char		uidl[80];	// used by pop3
	int		mesgno;
};

struct Header {
	char *type;
	void (*f)(Message*, Header*, char*);
	int len;
};

struct Inbuf
{
	int	fd;
	uchar	*lim;
	uchar	*rptr;
	uchar	*wptr;
	uchar	data[Buffersize+7];
};

void *		emalloc(ulong n);
void *		erealloc(void *p, ulong n);
int		headerline(char **pp, String *hl);
int		isattribute(char **pp, char *attr);
String*		promote(String **sp);
void		parseunix(Message *m);
void		parseunix(Message *m);
void		bcc822(Message *m, Header *h, char *p);
void		cc822(Message *m, Header *h, char *p);
void		cdisposition(Message *m, Header *h, char *p);
void		cencoding(Message *m, Header *h, char *p);
void		ctype(Message *m, Header *h, char *p);
void		date822(Message *m, Header *h, char *p);
void		from822(Message *m, Header *h, char *p);
void		inreplyto822(Message *m, Header *h, char *p);
void		messageid822(Message *m, Header *h, char *p);
void		mimeversion(Message *m, Header *h, char *p);
void		replyto822(Message *m, Header *h, char *p);
void		sender822(Message *m, Header *h, char *p);
void		subject822(Message *m, Header *h, char *p);
void		to822(Message *m, Header *h, char *p);
int		readmessage(Message *m, Inbuf *inb);
void		parseattachments(Message *m, Mailbox *mb);
void		parsebody(Message *m, Mailbox *mb);
void		parse(Message *m, int justmime, Mailbox *mb, int addfrom);
void		parseheaders(Message *m, int justmime, Mailbox *mb, int addfrom);
void		addtomessage(Message *m, uchar *p, int n, int done);
char*		readmbox(Mailbox *mb);
void		delmessage(Mailbox *mb, Message *m);
String*		addr822(char *p);
void		killtrailingwhite(char *p);
Mailbox*	newmbox(char *path, char *name, int std);
int		decquoted(char *out, char *in, char *e);

#define dprint if(debug)fprint

extern int debug;
