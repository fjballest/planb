#include <u.h>
#include <libc.h>
#include <error.h>

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

Error**	__ep;

void
errinit(Error* e)
{
	if (__ep == nil)
		__ep = privalloc();
	*__ep = e;
	memset(e, 0, sizeof(Error));
}

void
noerror(void)
{
	if ((*__ep)->nerr-- == 0)
		sysfatal("noerror w/o catcherror");
}

void
error(char* msg, ...)
{
	char	buf[500];
	va_list	arg;

	if (msg != nil){
		va_start(arg, msg);
		vseprint(buf, buf+sizeof(buf), msg, arg);
		va_end(arg);
		werrstr("%s", buf);
	}
	if ((*__ep)->nerr == 0)
		sysfatal("%s", buf);
	(*__ep)->nerr--;
	longjmp((*__ep)->label[(*__ep)->nerr], 1);
}

void
warn(char* msg, ...)
{
	char	buf[500];
	va_list	arg;

	assert(msg != nil);
	va_start(arg, msg);
	vseprint(buf, buf+sizeof(buf), msg, arg);
	va_end(arg);
	fprint(2, "%s: warning: %s\n", argv0, buf);
}
