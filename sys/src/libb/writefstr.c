#include <u.h>
#include <libc.h>
#include <b.h>

long		
writefstr(char* f, char* s)
{
	return writef(f, s, strlen(s));
}

