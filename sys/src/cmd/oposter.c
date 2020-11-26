#include <u.h>
#include <libc.h>
#include <thread.h>
#include <omero.h>
#include <plumb.h>
#include <error.h>
#include <draw.h>
#include <b.h>

Panel*	poster;

static void*
convimg(char* fn, long* lp)
{
	int	p[2];
	int	l;
	char*	cmd;
	char*	out;
	long	tot, n;
	long	nout;

	l = strlen(fn);
	if (l < 5)
		return nil;
	if (!cistrcmp(fn+l-4, ".gif"))
		cmd="/bin/gif";
	else if (!cistrcmp(fn+l-4, ".jpg"))
		cmd="/bin/jpg";
	else if (!cistrcmp(fn+l-4, ".png"))
		cmd="/bin/png";
	else if (!cistrcmp(fn+l-4, ".ppm"))
		cmd="/bin/ppm";
	else
		cmd= nil;
	if (cmd == nil)
		return nil;
	if (pipe(p) < 0)
		return nil;
	switch(rfork(RFPROC|RFFDG|RFNOWAIT)){
	case -1:
		close(p[0]);
		close(p[1]);
		break;
	case 0:
		close(p[0]);
		dup(p[1], 1);
		close(p[1]);
		execl(cmd, cmd, "-9", fn, nil);
		sysfatal("exec failed: %r");
		break;
	default:
		close(p[1]);
		out = emalloc(128*1024);
		nout = 128*1024;
		for(tot = 0; ; tot +=n){
			n = read(p[0], out+tot, nout - tot);
			if (n <= 0)
				break;
			if (n == nout - tot){
				nout += 128*1024;
				out = erealloc(out, nout);
			}
		}
		close(p[0]);
		*lp = tot;
		if (tot <= 0){
			free(out);
			out = nil;
		}
		return out;
	}
	return nil;
}


static void
showimg(Plumbmsg* m)
{
	void*	img;
	long	l;

	if (access(m->data, AREAD) < 0)
		return;
	img = convimg(m->data, &l);
	if (img){
		openpanel(poster, OWRITE|OTRUNC);
		writepanel(poster, img, l);
		closepanel(poster);
		openpanelctl(poster);
		panelctl(poster, "show\n");
		closepanelctl(poster);
		free(img);
	}
}

void
omerogone(void)
{
	fprint(2, "%s: terminated\n", argv0);
	threadexitsall(nil);
}

void
usage(void)
{
	fprint(2, "usage: %s [-d] [-p port]\n", argv0);
	threadexitsall("usage");
}

void
threadmain(int argc, char* argv[])
{
	Plumbmsg* m;
	int plumbfd;
	char*	port;

	port = "poster";
	ARGBEGIN{
	case 'd':
		omerodebug++;
		break;
	case 'p':
		port = EARGF(usage());
		break;
	default:
		fprint(2, "usage: %s \n", argv0);
		sysfatal("usage");
	}ARGEND;
	if (argc > 0){
		fprint(2, "usage: %s\n", argv0);
		sysfatal("usage");
	}
	plumbfd = createport(port);
	if (plumbfd < 0)
		sysfatal("poster port: %r");
	poster = createpanel("oposter", "page", nil);
	if (poster == nil)
		sysfatal("createpanel: %r\n");
	panelctl(poster, "tag\nhide\n");
	closepanelctl(poster);
	for(;;){
		m = plumbrecv(plumbfd);
		if(m == nil)
			break;
		showimg(m);
		plumbfree(m);
	}
	close(plumbfd);
	threadexitsall(nil);
}
