
typedef struct Cmouse Cmouse;

enum {
	// Cmouse flag
	CMquiet	= 1,
	CMdouble	= 2,
};

struct Cmouse{
	Mouse;		// as in raw mode.
	int	flag;	// none, quiet, or double.
};

extern int		Mfmt(Fmt*);
#pragma varargck	type "M"	Cmouse*
