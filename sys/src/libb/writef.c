#include <u.h>
#include <libc.h>
#include <b.h>

long		
writef(char* f, void* buf, long len)
{
	int	fd;
	long	r;

	// BUG: retry it all if we get io errors while writing.

	fd = open(f, OWRITE|OTRUNC);
	if (fd < 0)
		return -1;
	if (len > 0)
		r = write(fd, buf, len);
	else
		r = 0;
	close(fd);
	return r;

}
