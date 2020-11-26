/*
 * Yet more code duplicated to perform the amount op.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <auth.h>
#include <ip.h>
#include "names.h"
#include "vols.h"

static int
agetfrep(int fd, Frpc* fop)
{
	int r;

	r = getfrep(fd, fop);
	if (r > 0 || r == -1)
		Dbgprint(fop->fid, "   <= %F\n", &fop->r);
	return r;
}

static long
fswrite(int fd, int fid, Frpc* fop, void* a, long n)
{
	fop->f.tag = 0xb;
	fop->f.type= Twrite;
	fop->f.fid = fid;
	fop->f.data = a;
	fop->f.count = n;
	if (putfcall(fd, fop) <= 0 || agetfrep(fd, fop) <= 0 ||
	    fop->r.type != Rwrite || fop->r.tag != fop->f.tag)
		return -1;
	return fop->r.count;
}

static long
fsread(int fd, int fid, Frpc* fop, void* a, long n)
{
	fop->f.tag = 0xc;
	fop->f.type= Tread;
	fop->f.fid = fid;
	fop->f.count = n;
	if (putfcall(fd, fop) <= 0 || agetfrep(fd, fop) <= 0 ||
	    fop->r.type != Rread || fop->r.tag != fop->f.tag)
		return -1;
	memmove(a, fop->r.data, fop->r.count);
	return fop->r.count;
}

static long
fsclose(int fd, int fid, Frpc* fop)
{
	fop->f.tag = 0xd;
	fop->f.type= Tclunk;
	fop->f.fid = fid;
	if (putfcall(fd, fop) <= 0 || agetfrep(fd, fop) <= 0 ||
	    fop->r.type != Rclunk || fop->r.tag != fop->f.tag)
		return -1;
	return 0;
}

static int
fsattach(int fd, int fid, int afid, char* spec, Frpc* fop, Qid* q)
{
	fop->f.tag = 0xe;
	fop->f.type= Tattach;
	fop->f.fid = fid;
	fop->f.afid= afid;
	fop->f.uname = getuser();
	fop->f.aname = spec;
	if (putfcall(fd, fop) <= 0 || agetfrep(fd, fop) <= 0 ||
	    fop->r.type != Rattach || fop->r.tag != fop->f.tag)
		return -1;
	*q = fop->r.qid;
	return 0;
}

static char* anames[] = {
	"ok", "done", "error", "needkey", "badkey", "writenext", "toosmall", "toobig",
	"rpcfailure", "phase" };

static AuthInfo*
fsfauth_proxy(int fsfd, int afid, Frpc* fop, AuthRpc *rpc, char* params)
{
	char *buf;
	int m, n, ret;
	AuthInfo *a;
	char oerr[ERRMAX];
	int	id;

	rerrstr(oerr, sizeof oerr);
	werrstr("UNKNOWN AUTH ERROR");

	assert(rpc);
	if(auth_rpc(rpc, "start", params, strlen(params)) != ARok){
		werrstr("fauth_proxy start: %r");
		return nil;
	}

	buf = emalloc(AuthRpcMax);
	for(;;){
		id = auth_rpc(rpc, "read", nil, 0);
		switch(id){
		case ARdone:
			free(buf);
			a = auth_getinfo(rpc);
			errstr(oerr, sizeof oerr);	/* no error, restore whatever was there */
			return a;
		case ARok:
			if(fswrite(fsfd, afid, fop, rpc->arg, rpc->narg) != rpc->narg){
				werrstr("auth_proxy write fd: %r");
				goto Error;
			}
			break;
		case ARphase:
			n = 0;
			memset(buf, 0, AuthRpcMax);
			while((ret = auth_rpc(rpc, "write", buf, n)) == ARtoosmall){
				if(atoi(rpc->arg) > AuthRpcMax)
					break;
				m = fsread(fsfd, afid, fop, buf+n, atoi(rpc->arg)-n);
				if(m <= 0){
					if(m == 0)
						werrstr("auth_proxy short read: %s", buf);
					goto Error;
				}
				n += m;
			}
			if (ret != ARok){
				goto Error;
			}
			break;
		default:
			werrstr("auth_proxy rpc: %r");
			goto Error;
		}
	}
Error:
	free(buf);
	return nil;
}


static AuthInfo*
fsauth_proxy(int fsfd, int afid, Frpc* fop, char* params)
{
	AuthInfo *ai;
	AuthRpc *rpc;
	int	afd;

	quotefmtinstall();	/* just in case */
	afd = open("/mnt/factotum/rpc", ORDWR);
	if(afd < 0){
		// Try mounting it.
		afd = open("#s/factotum", ORDWR);
		if (afd < 0){
			return nil;
		}
		mount(afd, -1, "/mnt", MREPL, "");
		afd = open("/mnt/factotum/rpc", ORDWR);
		if (afd < 0){
			return nil;
		}
	}
	rpc = auth_allocrpc(afd);
	if(rpc == nil) {
		close(afd);
		return nil;
	}
	ai = fsfauth_proxy(fsfd, afid, fop, rpc, params);
	auth_freerpc(rpc);
	close(afd);
	return ai;
}

static int
authfsfd(int fd, char* spec, Frpc* fop)
{
	AuthInfo*ai;
	int	afid;

	fop->f.tag = 0xb;
	fop->f.type= Tauth;
	fop->f.afid = getfidnr();
	fop->f.uname = getuser();
	fop->f.aname = spec;
	afid = -1;
	if (putfcall(fd, fop) <= 0 || agetfrep(fd, fop) <= 0)
		goto fail;
	if (fop->r.type == Rerror)  // no auth required.
		goto done;
	if (fop->r.type != Rauth || fop->r.tag != fop->f.tag)
		goto fail;
	else
		afid = fop->f.afid;
	/* The next call depends on linking against our own
	 * fauth_proxy function, which speaks 9P to the server
	 * instead of using the kernel.
	 */
	ai = fsauth_proxy(fd, afid, fop, "proto=p9any role=client");
	if (0 && ai == nil) // Debugging this
		goto fail;
	auth_freeAI(ai);
done:
	return afid;
fail:
	close(fd);
	return -1;

}

static int
fsversion(int fd, Frpc* fop)
{
	fop->f.tag = NOTAG;
	fop->f.type= Tversion;
	fop->f.msize= Maxmsglen;
	fop->f.version = "9P2000";
	if (putfcall(fd, fop) <= 0 || agetfrep(fd, fop) <= 0){
		vdprint(2, "can't RPC Tversion: %r\n");
		return -1;
	}
	if (fop->r.tag != NOTAG || fop->r.type != Rversion){
		vdprint(2, "bad reply for Tversion");
		return -1;
	}
	if (strcmp(fop->r.version, "9P2000")){
		vdprint(2, "bad server version\n");
		return -1;
	}
	if (fop->r.msize < Maxmsglen)
		msglen = fop->r.msize;
	return 1;
}

static int
dialfs(Fs* fs)
{
	int	fd, cfd;
	char	dir[50];
	char*	s;

	cfd = -1;
	if (fs->addr[0] == '#' || fs->addr[0] == '/')
		fd = open(fs->addr, ORDWR);
	else {
		s=smprint("/net/%s", fs->addr);
		fd = dial(s, 0, dir, &cfd);
		free(s);
		if (cfd != -1){
			fprint(cfd, "keepalive 2000\n");
			close(cfd);
		}
	}
	return fd;
}

int
amountfs(Fs* fs)
{
	int	fd, afid, r;
	Frpc*	fop;
	int	doversion;
	Dir*	d;

	fd = dialfs(fs);
	if (fd < 0){
		vdprint(2, "%s: can't dial %s: %r\n", argv0, fs->addr);
		return 0;
	}
	if (fs->addr[0] != '#' && fs->addr[0] != '/')
		doversion = 1;
	else {
		d = dirfstat(fd);
		if (d == nil)
			return 0;
		doversion = (d->qid.path != fs->fdqid.path);
		fs->fdqid = d->qid;
		free(d);
	}
	fop = rpcalloc();
	if (doversion && fsversion(fd, fop) < 0){
		vdprint(2, "%s: %s: bad fversion\n", argv0, fs->addr);
		close(fd);
		rpcfree(fop);
		return 0;
	}
	afid = authfsfd(fd, fs->spec, fop);
	if (fsattach(fd, fs->fid->snr, afid, fs->spec, fop, &fs->fid->qid) < 0){
		vdprint(2, "%s: attach failed: %s\n", argv0, fs->addr);
		close(fd); // afid will be collected by the server.
		r = 0;
	} else {
		if (afid >= 0)
			fsclose(fd, afid, fop);
		fs->fid->qid.path |= fs->qid.path;
		fs->fd = fd;	// make it official.
		r = 1;
	}
	rpcfree(fop);
	return r;
}

