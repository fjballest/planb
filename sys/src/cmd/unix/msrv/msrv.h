
enum{
	Stdxmax = 1280,
	Stdymax = 1024,
	Click = 2,
	Down = 1,
	Nothing = 0,
	Up = -1
};

typedef struct Mov Mov;
struct Mov{
	int isquit;
	char dev;
	int x;
	int y;
	int but[5];
};

extern int buttons;
extern int verbose;

int initdisplay();
int closedisplay();
int sendmov(Mov *m);
