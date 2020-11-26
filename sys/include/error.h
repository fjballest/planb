#pragma	lib	"liberror.a"
#pragma src "/sys/src/liberror"

typedef struct Error Error;

enum {
	Nerrors	= 32,
};

struct Error {
	jmp_buf label[Nerrors];
	int	nerr;
};

char*	estrdup(char*);
void*	emalloc(int);
void*	erealloc(void*,int);
void	errinit(Error* e);
void	noerror(void);
void	error(char* msg, ...);
void	warn(char* msg, ...);
#define	catcherror()	setjmp((*__ep)->label[(*__ep)->nerr++])
extern Error**	__ep;
