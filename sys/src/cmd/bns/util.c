#include <u.h>
#include <libc.h>
 
int
postfd(char *name, int pfd)
{
	int fd;
	char buf[80];

	snprint(buf, sizeof buf, "/srv/%s", name);
	fd = create(buf, OWRITE|ORCLOSE|OCEXEC, 0600);
	if(fd < 0)
		return -1;
	if(fprint(fd, "%d", pfd) < 0){
		close(fd);
		return -1;
	}
	return 0;
}

char*
estrdup(char* s)
{
	s = strdup(s);
	if (s == nil)
		sysfatal("estrdup: not enough memory");
	setmalloctag(s, getcallerpc(&s));
	return s;
}

void*
emalloc(int sz)
{
	void*	s;

	s = malloc(sz);
	if (s == nil)
		sysfatal("emalloc: not enough memory");
	setmalloctag(s, getcallerpc(&sz));
	return s;
}

void*
erealloc(void* p, int sz)
{

	p = realloc(p, sz);
	if (p == nil)
		sysfatal("erealloc: not enough memory");
	return p;
}
