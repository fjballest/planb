/*
 * 9P file server for an empty directory.
 * To deal with gone volumes.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <auth.h>
#include <ip.h>
#include "names.h"
#include "vols.h"

static Dir*
emptydirstat(void)
{
	Dir*	d;
	char*	s;
	char*	n;

	d = emalloc(sizeof(Dir)+ 4 + 4 + 4 + 4);
	memset(d, 0, sizeof(Dir));
	s = (char*)d;
	n = s + sizeof(Dir);
	d->name = n;	strcpy(n, "vol");	n += 4;
	d->uid = n;	strcpy(n, "sys");	n += 4;
	d->gid = n;	strcpy(n, "sys");	n += 4;
	d->muid = n;	strcpy(n, "sys");
	d->qid.type = QTDIR;
	d->qid.path = 0;
	d->mode = DMDIR|0555;
	return d;
}

static int
statop(Fs* , Frpc* fop)
{
	fop->r.tag = fop->f.tag;
	fop->r.type= Rstat;
	fop->d = emptydirstat();
	return 1;
}

static int
walkop(Fs* , Frpc* m)
{
	Qid q;
	int	i;

	m->r.tag = m->f.tag;
	m->r.type = Rwalk;
	q.type = QTDIR;
	q.path = 0;
	q.vers = 0;
	m->r.nwqid = 0;
	for(i = 0; i < m->f.nwname && i < MAXWELEM; i++)
		if(strcmp(m->f.wname[i], "..") == 0)
			m->r.wqid[m->r.nwqid++] = q;
		else
			break;
	if (!m->f.nwname || m->r.nwqid > 0)
		return 1;
	rpcerr(m, Enotexist);
	return 0;
}

static int
readop(Fs* , Frpc* fop)
{
	fop->r.tag = fop->f.tag;
	fop->r.type= Rread;
	fop->r.count = 0;
	return fop->r.count;
}

static int
clunkop(Fs*, Frpc* fop)
{
	fop->r.tag = fop->f.tag;
	fop->r.type= Rclunk;
	return 0;
}

static int
openop(Fs*, Frpc* fop)
{
	fop->r.tag = fop->f.tag;
	fop->r.type= Ropen;
	fop->r.qid.path = DMDIR;
	fop->r.qid.type = QTDIR;
	fop->r.qid.vers = 0;
	fop->r.iounit = 8*1024;
	return 0;
}

int
noop(Fs* , Frpc* fop)
{
	rpcerr(fop, Eperm);
	return -1;
}

typedef int (*Frpcf)(Fs*, Frpc*);
Frpcf	nilfsops[Tmax] = {
	[Topen]		openop,
	[Tcreate]	noop,
	[Tclunk]	clunkop,
	[Tread]		readop,
	[Twalk]		walkop,
	[Twrite]	noop,
	[Tremove]	noop,
	[Tstat]		statop,
	[Twstat]	noop,
};
