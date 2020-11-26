#include <u.h>
#include <libc.h>

enum {
	Hitmout	= 60 * 60,	// power off time out (secs)
	Lotmout = 60,		// lights off
	Ival	= 5,		// check interval (secs)

	// power states
	Sunknown = 0,
	Sidle,	// no who, no pwr
	Son,	// who, pwr
	Stmout,	// pwr timing out
	Nstates,
};

typedef struct Cmd Cmd;
typedef struct State State;
typedef int (*Statef)(Cmd* c, State* s);

struct State {
	vlong	gonetime;
	int	who;
	int	lastwho;
	vlong	now;
};

struct Cmd {
	vlong	gonetime;
	char*	file;
	int	state;
	int	last;
	int	tmout;
	Statef	func[Nstates];
};



int
getx10(char* f)
{
	int	fd;
	char	buf[10];
	int	n;

	fd = open(f, OREAD);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf)-1);
	if (n < 0){
		close(fd);
		return -1;
	}
	buf[n] = 0;
	close(fd);
	if (strcmp(buf, "on") == 0)
		return 1;
	else
		return 0;
}

void
setx10(char* f, char* to)
{
	int	fd;
	int	n;

	if (f == nil)
		return;
	fd = open(f, OWRITE);
	if (fd < 0){
		fprint(2, "%s: error: %s -> %s: %r\n", argv0, f, to);
		syslog(0, "x10a", "%s: open error: %s -> %s: %r\n", argv0, f, to);
		return;
	}
	n = write(fd, to, strlen(to));
	close(fd);
	if (n < 0)
		syslog(0, "x10a", "%s: write error: %s -> %s: %r\n", argv0, f, to);
}

static void
switchdev(char* file, char* what, int tmout)
{
	if (tmout > 0){
		syslog(0, "x10a", "%s: switching %s"
			" %s in %d minutes", argv0, what, file, tmout/60);
	} else {
		syslog(0, "x10a", "%s: switching %s %s now", argv0, what, file);
		setx10(file, what);
		sleep(1000);
	}
}

void
setvol(char* file, int val)
{
	int	fd;
	fd = open(file, OWRITE);
	if (fd < 0)
		return;
		syslog(0, "x10a", "%s: adjusting %s to %d", argv0, file, val);
	fprint(fd, "audio out %d\n", val);
	close(fd);
}

int
getvol(char* file)
{
	char	buf[100];
	int	fd;
	int	n;
	int	l;

	fd = open(file, OREAD);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf)-1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	l = strlen("audio out ");
	if (strncmp(buf, "audio out ", l) != 0)
		return 70;
	return atoi(buf + l);
}

	

static int 
terminit(Cmd* c, State* s)
{
	if (s->who){
		switchdev(c->file, "on", 0);
		return Son;
	} else {
		s->gonetime = s->now;
		switchdev(c->file, "off", c->tmout);
		return Stmout;
	}
}

static int 
volinit(Cmd* c, State* s)
{
	if (c->last < 0)
		c->last = getvol(c->file);
	if (s->who){
		return Son;
	} else {
		return Stmout;
	}
}

static int 
nvolinit(Cmd* c, State* s)
{
	c->last = getvol(c->file);
	if (s->who){
		if (c->last > 40)
			setvol(c->file, 40);
		return Son;
	} else {
		if (c->last > 0)
			setvol(c->file, c->last);
		return Sidle;
	}
}


static int 
termidle(Cmd* c, State* s)
{
	if (s->who && !s->lastwho){
		switchdev(c->file, "on", 0);
		return Son;
	} else
		return Sidle;
}

static int 
volidle(Cmd* c, State* s)
{
	if (c->last < 0)
		c->last = 70;
	if (s->who && !s->lastwho){
		setvol(c->file, c->last);
		return Son;
	} else
		return Sidle;
}

static int 
nvolidle(Cmd* c, State* s)
{
	if (s->who && !s->lastwho){
		c->last = getvol(c->file);
		if (c->last == 0){
			sleep(1000);
			c->last = getvol(c->file);
		}
		if (c->last > 70)
			setvol(c->file, 70);
		return Son;
	} else
		return Sidle;
}

static int 
termon(Cmd* c, State* s)
{
	if (!s->who && s->lastwho){
		switchdev(c->file, "off", c->tmout);
		return Stmout;
	} else
		return Son;
}

static int 
volon(Cmd* c, State* s)
{
	if (!s->who && s->lastwho){
		c->last = getvol(c->file);
		return Stmout;
	} else
		return Son;
}

static int 
nvolon(Cmd* c, State* s)
{
	if (!s->who && s->lastwho){
		if (c->last == 0){
			c->last = getvol(c->file);
		}
		if (c->last > 0)
			setvol(c->file, c->last);
		return Sidle;
	} else
		return Son;
}

static int 
termtmout(Cmd* c, State* s)
{
	if (s->who && !s->lastwho){
		switchdev(c->file, "on", 0);
		return Son;
	} else if (s->gonetime > 0 && s->now - s->gonetime >= c->tmout){
		switchdev(c->file, "off", 0);
		c->gonetime = 0;	// no more power offs until he cames
		return Sidle;
	} else
		return Stmout;
}

static int 
voltmout(Cmd* c, State* s)
{
	if (s->who && !s->lastwho){
		if (c->last == 0)
			c->last = 70;
		setvol(c->file, c->last);
		return Son;
	} else if (s->gonetime > 0 && s->now - s->gonetime >= c->tmout){
		c->last = getvol(c->file);
		setvol(c->file, 0);
		s->gonetime = 0;	// no mutes until he cames
		return Sidle;
	} else
		return Stmout;
}

Cmd	tcmd = {
	0LL, nil, Sunknown, -1, Hitmout, {terminit,	termidle,	termon,	termtmout}
};
Cmd	lcmd = {
	0LL, nil, Sunknown, -1, Lotmout, {terminit,	termidle,	termon,	termtmout}
};
Cmd	vcmd = {
	0LL, nil, Sunknown, -1, Lotmout, {volinit,	volidle,	volon,	voltmout}
};
Cmd	ncmd = {
	0LL, nil, Sunknown, -1, Lotmout, {nvolinit,	nvolidle,	nvolon,	nil}
};

Cmd	cmds[100];
int	ncmds;

void
usage(void)
{
	fprint(2, "usage: %s "
		"[-t term] [-n vol] [-v vol] [-l light] whofile\n", argv0);
	exits("usage");
}

void
main(int argc, char* argv[])
{
	State	s;
	char*	whof;
	Cmd*	cp;
	int	i;

	ARGBEGIN{
	case 't':
		cmds[ncmds] = tcmd;
		cmds[ncmds].file = EARGF(usage());
		ncmds++;
		break;
	case 'v':
		cmds[ncmds] = vcmd;
		cmds[ncmds].file = EARGF(usage());
		ncmds++;
		break;
	case 'n':
		cmds[ncmds] = ncmd;
		cmds[ncmds].file = EARGF(usage());
		ncmds++;
		break;
	case 'l':
		cmds[ncmds] = lcmd;
		cmds[ncmds].file = EARGF(usage());
		ncmds++;
		break;
	default:
		usage();
	}ARGEND;
	if (argc != 1 || ncmds == 0)
		usage();
	whof = argv[0];

	memset(&s, 0, sizeof(s));
	for(;;){
		sleep(Ival * 1000);

		s.now = time(nil);
		s.lastwho = s.who;
		s.who = getx10(whof);
		if (s.who < 0){
			syslog(0, "x10a", "%s: can't read %s", argv0, whof);
			continue;
		}
		if (s.who != s.lastwho){
			if (s.who){
				for (i = 0; i < ncmds; i++)
					cmds[i].gonetime = 0;
				//syslog(0, "x10a", "%s: %s came", argv0, whof);
				s.gonetime = 0;
			} else {
				for (i = 0; i < ncmds; i++)
					cmds[i].gonetime = s.now;
				//syslog(0, "x10a", "%s: %s gone", argv0, whof);
				s.gonetime = s.now;
			}
		}

		for (i = 0; i < ncmds; i++){
			cp = &cmds[i];
			assert(cp->state >= 0 && cp->state < Nstates);
			cp->state = cp->func[cp->state](cp, &s);
		}
	}
}
