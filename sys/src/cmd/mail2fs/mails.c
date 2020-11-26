#include <u.h>
#include <ctype.h>
#include <libc.h>
#include <b.h>

int showarch;
int showspam;

void
usage(void)
{
	fprint(2, "usage: %s [-aAs] [dir]\n", argv0);
	exits("usage");
}

static int
cmpent(void* a1, void* a2)
{
	Dir*	d1 = a1;
	Dir*	d2 = a2;
	int	n1, n2;

	if (d1->name[0] == 'a' && d1->name[1] == '.')
		n1 = atoi(d1->name + 2);
	else
		n1 = atoi(d1->name);
	if (d2->name[0] == 'a' && d2->name[1] == '.')
		n2 = atoi(d2->name + 2);
	else
		n2 = atoi(d2->name);

	return n2 - n1;
}

char*
gethdr(char** hdrs, char* h)
{
	int	l;

	l = strlen(h);
	while(*hdrs){
		//fprint(2, "hdr %s\n", *hdrs);
		if (cistrncmp(*hdrs, h, l) == 0)
			return *hdrs + l + 1;
		hdrs++;
	}
	return "<none>";
}

void
hdrline(char*fn, char* buf)
{
	char*	hdrs[10+1];
	int	nhdrs;
	char*	f;
	char*	s;
	Dir*	de;
	int	n;
	int	fd;
	int	i;

	s = buf;
	for (i = 0; s && i < 10; i++){
		f = utfrune(s, '\n');
		if (f)
			*f++ = 0;
		hdrs[i] = s;
		s = f;
	}
	nhdrs = i;
	hdrs[nhdrs] = nil;

	f = gethdr(hdrs, "from");
	s = gethdr(hdrs, "subject");
	print("%-18.18s %-15.15s %-50.50s\n", fn, f, s);
	fd = open(fn, OREAD);
	n = dirreadall(fd, &de);
	if (n > 1)
		print("\t");
	for (i = 0; i < n; i++)
		if (strcmp(de[i].name, "text") != 0)
			print("\t%s/%s\n", fn, de[i].name);
}

int
mustshow(char* name)
{
	if (isdigit(name[0]))
		return 1;
	if (showarch && name[0] == 'a' && name[1] == '.')
		return 1;
	if (showspam && name[0] == 's' && name[1] == '.')
		return 1;
	return 0;
}

int
list(char* dir)
{
	static char	buf[1024];
	Dir*	d;
	int	n;
	int	fd;
	int	i; 
	long	l;
	char*	tf;
	int	some;
	char*	suf;

	fd = open(dir, OREAD);
	if (fd < 0){
		fprint(2, "%s: %r\n", dir);
		return 0;
	}
	n = dirreadall(fd, &d);
	close(fd);
	if (n <= 0)
		return 0;
	some = 0;
	qsort(d, n, sizeof(Dir), cmpent);
	for (i = 0; i < n; i++)
		if (mustshow(d[i].name)){
			some++;
			tf = smprint("%s/%s/text", dir, d[i].name);
			readf(tf, buf, sizeof(buf), &l);
			buf[sizeof(buf)-1] = 0;
			for (suf = tf + strlen(dir) - 1; *suf != '/' && suf > tf; suf--)
				;
			suf++;
			hdrline(suf, buf);
			free(tf);
		}
	free(d);
	return some;
}

int
listmbox(char* mbox)
{
	Dir*	d;
	int	fd;
	int	i;
	int	n;
	int	some;
	char*	dir;

	fd = open(mbox, OREAD);
	if (fd < 0){
		fprint(2, "%s: %r\n", mbox);
		return 0;
	}
	n = dirreadall(fd, &d);
	close(fd);
	if (n <= 0)
		return 0;
	some = 0;
	qsort(d, n, sizeof(Dir), cmpent);
	for (i = 0; i < n; i++)
		if (d[i].qid.type&QTDIR){
			dir=smprint("%s/%s", mbox, d[i].name);
			some |= list(dir);
			free(dir);
		}
	free(d);
	return some;
}

char*
cleanpath(char* file, char* dir)
{
	char*	s;
	char*	t;

	assert(file && file[0]);
	if (file[1])
		file = strdup(file);
	else {
		s = file;
		file = malloc(3);
		file[0] = s[0];
		file[1] = 0;
	}
	s = cleanname(file);
	if (s[0] != '/' && dir != nil && dir[0] != 0){
		t = smprint("%s/%s", dir, s);
		free(s);
		s = cleanname(t);
	}
	return s;
}

void
main(int argc, char*argv[])
{
	char*	dir;
	char*	top;

	ARGBEGIN{
	case 'a':
		showarch++;
		break;
	case 's':
		showspam++;
		break;
	case 'A':
		showarch++;
		showspam++;
		break;
	default:
		usage();
	}ARGEND;
	if (argc == 1){
		top = smprint("/mail/box/%s", getuser());
		dir = cleanpath(argv[0], top);
	} else {
		if (argc != 0)
			usage();
		dir = smprint("/mail/box/%s/mails", getuser());
	}
	if (!listmbox(dir))
		print("No mail\n");
	exits(nil);
}
