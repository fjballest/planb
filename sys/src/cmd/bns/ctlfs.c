/*
 * 9P file server for the control file system.
 * This provides a control file system with just
 * a vol file to get/set status for volumes.
 *
 * BUG: there is no way (yet) to update constraints.
 * This requires the addition of a "set" command.
 */

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <auth.h>
#include <ip.h>
#include "names.h"
#include "vols.h"

Fs	ctlfs;
uvlong	Ctldirqid=	0x000000008c000001ULL;
uvlong	Ctlqid=		0x000000008d000000ULL;

static Dir*
ctldirstat(int isdir)
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

	// depends on the qid.
	if (isdir){
		d->qid.type = QTDIR;
		d->qid.path = Ctldirqid;
		d->mode = DMDIR|0555;
	} else {
		d->qid.type = 0;
		d->qid.path = Ctlqid;
		d->qid.vers = epoch.ref;
		d->mode = 0660;
	}
	return d;
}

static int
statop(Fs* , Frpc* fop)
{
	
	fop->r.tag = fop->f.tag;
	fop->r.type= Rstat;
	fop->d = ctldirstat(fop->fid->qid.type&QTDIR);
	return 1;
}

static char*
volsmprint(void)
{
	static char buf[16*1024];
	int	i;
	char*	s;
	Vol*	v;

	qlock(&volslck);
	s = buf;
	*s = 0;
	for (i = 0; i < nvols; i++){
		v = vols[i];
		if (v->dead && !v->disabled)
			continue;
		if (v->dead || v->disabled)
			s = seprint(s, buf+sizeof(buf), "#%V ", v);
		else
			s = seprint(s, buf+sizeof(buf), "%V ", v);
		if (vols[i]->disabled)
			s = seprint(s, buf+sizeof(buf), "# disabled");
		else if (vols[i]->dead)
			s = seprint(s, buf+sizeof(buf), "# dead");
		s = seprint(s, buf+sizeof(buf), "\n");
	}
	qunlock(&volslck);
	qlock(&mvolslck);
	for (i = 0; i < nmvols; i++)
		if (mvols[i])
			s = seprint(s, buf+sizeof(buf), "# %W\n", mvols[i]);
	qunlock(&mvolslck);
	return estrdup(buf);
}

static int
openop(Fs*, Frpc* fop)
{
	Fid*	fid;
	static Dir* root;

	if (root == nil)
		root = ctldirstat(0);

	fid = fop->fid;
	fop->r.tag= fop->f.tag;
	if (fid->qid.type&QTDIR){
		if ((fop->f.mode&3) != OREAD){
			rpcerr(fop, Eperm);
			return -1;
		}
		if (fid->ureadbuf == nil){
			fid->ureadbuf = emalloc(128);
			fid->ureadlen = convD2M(root, (uchar*)fid->ureadbuf, 128);
		}
	} else {
		fid->ureadbuf = volsmprint();
		fid->ureadlen = strlen(fid->ureadbuf);
	}
	fop->r.type= Ropen;
	fop->r.qid = fid->qid;
	fop->r.iounit = 8*1024;
	return 0;
}

static int
readop(Fs* , Frpc* m)
{
	long	off, len;

	m->r.tag = m->f.tag;
	m->r.type= Rread;
	m->r.data= (char*)m->buf;
	m->r.count = 0;
	off = m->f.offset;
	len = m->fid->ureadlen;
	if (off >= len || !m->f.count)
		return 0;
	if (m->fid->qid.type&QTDIR){
		if (m->f.count >= len && off == 0){
			memmove(m->r.data, m->fid->ureadbuf, len);
			m->r.count = len;
		}
	} else {
		if (off + m->f.count > len)
			m->f.count = len - off;
		m->r.count = m->f.count;
		memmove(m->r.data, m->fid->ureadbuf+off, m->r.count);
	}
	return m->r.count;
}

static int
writeop(Fs* fs, Frpc* fop)
{
	int	n;

	if (fop->fid->qid.type&QTDIR)
		return noop(fs, fop);
	fop->r.tag = fop->f.tag;
	fop->r.type= Rwrite;
	n = fop->f.count;
	if (n < 0)
		n = 0;
	if (n > 1024)
		n = 1024;
	fop->r.count = n;
	fop->f.data[n] = 0;
	cmdline(fop->f.data, "/dev/vol", 0);
	return n;
}

static int
walkop(Fs* , Frpc* m)
{
	int	dir;
	int	i;

	dir = (m->fid->qid.type&QTDIR);
	m->r.tag = m->f.tag;
	m->r.type = Rwalk;
	m->r.nwqid = 0;
	for(i = 0; i < m->f.nwname && i < MAXWELEM; i++)
		if(strcmp(m->f.wname[i], "..") == 0){
			dir = 1;
			m->r.wqid[m->r.nwqid].path = Ctldirqid;
			m->r.wqid[m->r.nwqid++].type = QTDIR;
		} else if (dir && !strcmp(m->f.wname[i], "vol")){
			dir = 0;
			m->r.wqid[m->r.nwqid].path = Ctlqid;
			m->r.wqid[m->r.nwqid++].type = 0;
		} else
			break;
	if (!m->f.nwname || m->r.nwqid > 0)
		return 1;
	rpcerr(m, Enotexist);
	return 0;
}

static int
clunkop(Fs*, Frpc* fop)
{
	free(fop->fid->ureadbuf);
	fop->fid->ureadbuf = nil;
	fop->r.tag = fop->f.tag;
	fop->r.type= Rclunk;
	return 0;
}

static int
flushop(Fs*, Frpc* fop)
{
	fop->r.tag = fop->f.tag;
	fop->r.type= Rflush;
	return 0;
}

typedef int (*Frpcf)(Fs*, Frpc*);
Frpcf	ctlfsops[Tmax] = {
	[Topen]		openop,
	[Twalk]		walkop,
	[Tcreate]	noop,
	[Tclunk]	clunkop,
	[Tread]		readop,
	[Twrite]	writeop,
	[Tremove]	noop,
	[Tstat]		statop,
	[Twstat]	noop,
	[Tflush]	flushop,
};
