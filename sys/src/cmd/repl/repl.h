typedef struct Db	Db;
typedef struct Dbent	Dbent;
typedef struct Meta	Meta;


enum {
	Uidlen	= 10,
	Uidslen	= 2 * Uidlen,
	Incr	= 16,

};

enum {
	// cmpfile and syncfile values
	Eq = 0,
	Metachg,
	Datachg,
};


struct Meta {
	char*	uid;
	char*	gid;
	ulong	mode;
	ulong	mtime;
	vlong	length;
	ulong	vers;	// of qid
};

struct Dbent {
	Avl	a;
	Dbent*	next;		// to link while outside the avl
	char*	fname;		// stored in disk
	char*	hist;		// stored in disk
	int	hlen;
	int	visited;
	int	pruned;
	Meta;			// stored in disk
};

struct Db {
	Avltree*tree;
	char	id;
	char*	dir;
	Dbent*	new;		// waiting for insert into tee.
	ulong	mtime;		// for last scan
	int	changed;
};

Db*	newdb(char id);
Db*	readdb(Biobufhdr* bp);
void	writedb(Biobufhdr* bp, Db* db);
void	insertdb(Db* db, Dbent* dp);
Db*	readdbfile(char* file);
void	writedbfile(char* file, Db* db);
void	rename(char* to, char*frompath);

