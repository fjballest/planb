#include <u.h>
#include <libc.h>
#include <b.h>

void*
readf(char*f, void* buf, long n, long* nout)
{
	char	err[ERRMAX];
	Dir*	d;
	char*	mbuf;
	int	fd;

	fd = open(f, OREAD);
	if (fd < 0)
		return nil;
	mbuf = nil;
	if (nout == nil)
		nout = &n;
	d = dirfstat(fd);
	if (d == nil)
		goto fail;

	if (buf == nil){
		n = d->length;
		if (n == 0)
			n = 16 * 1024; // Next read from a stream.
		mbuf = buf = malloc(n + 1);
		if (mbuf == nil)
			goto fail;
	}

	if (d->length == 0)
		*nout = read(fd, buf, n);
	else
		*nout = readn(fd, buf, n);
	if (*nout < 0)
		goto fail;
	if (mbuf != nil)
		mbuf[*nout] = 0;
	free(d);
	close(fd);
	return buf;
fail:
	rerrstr(err, sizeof(err));
	free(mbuf);
	free(d);
	close(fd);
	werrstr(err);
	*nout = -1;
	return nil;
}
