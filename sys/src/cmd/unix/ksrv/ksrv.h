typedef struct Key Key;
struct Key{
	char orig[sizeof(int)];	//UTFmax+1 to fit in an integer, trick for translation
	uint rune;
	uint keysym;
	uint keycode;
	int isshift;
	int iscontrol;
	int isaltgr;
};

extern int verbose;
extern int debug;

int initdisplay();
int closedisplay();
int sendkey(Key *k);
void translate(Key *k);
void xtranslate(Key *k);
