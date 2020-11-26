/*
 * Path names.
 */

typedef struct Name Name;

struct Name {
	// may be used from outside...
	char**	elems;	// element list, like in walk(5)
	int	nelems;	// # of used elems

	// implementation...
	int	aelems;	// # of allocated elems
	char*	base;	// memory for elements
	char*	end;	// end of allocated memory
	char*	ptr;	// ptr into end of used memory
};

Name*	n_new(void);
void	n_reset(Name*);
void	n_append(Name*, char*);
void	n_copy(Name* cn, Name* n);
int	n_eq(Name* n1, Name* n2);
void	n_free(Name*);
void	n_getpos(Name*, int* a, int* b);
void	n_setpos(Name*,	int a, int b);

int	namefmt(Fmt*);
#pragma     varargck    type  "N"   Name*
