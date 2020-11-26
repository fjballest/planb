#include "common.h"
#include <libsec.h>
#include <ctype.h>
#include "all.h"

/*
 * This is a kludge, at best.
 * All the horrors of mail processing are kept here, copied
 * from upas/fs and other places.
 * Cleanup is required, although it may not be feasible to
 * do so unless the world decides to start using a decent
 * format to encode mails.
 */

enum
{
	Mhead=	11,	/* offset of first mime header */
};

Header head[] =
{
	{ "date:", date822, },
	{ "from:", from822, },
	{ "to:", to822, },
	{ "sender:", sender822, },
	{ "reply-to:", replyto822, },
	{ "subject:", subject822, },
	{ "cc:", cc822, },
	{ "bcc:", bcc822, },
	{ "in-reply-to:", inreplyto822, },
	{ "mime-version:", mimeversion, },
	{ "message-id:", messageid822, },

[Mhead]	{ "content-type:", ctype, },
	{ "content-transfer-encoding:", cencoding, },
	{ "content-disposition:", cdisposition, },
	{ 0, },
};

static void
initheaders(void)
{
	Header *h;
	static int already;

	if(already)
		return;
	already = 1;

	for(h = head; h->type != nil; h++)
		h->len = strlen(h->type);
}

int
cistrncmp(char *a, char *b, int n)
{
	while(n-- > 0){
		if(tolower(*a++) != tolower(*b++))
			return -1;
	}
	return 0;
}

int
cistrcmp(char *a, char *b)
{
	for(;;){
		if(tolower(*a) != tolower(*b++))
			return -1;
		if(*a++ == 0)
			break;
	}
	return 0;
}

static char*
skipwhite(char *p)
{
	while(isspace(*p))
		p++;
	return p;
}

static char*
skiptosemi(char *p)
{
	while(*p && *p != ';')
		p++;
	while(*p == ';' || isspace(*p))
		p++;
	return p;
}

static char*
getstring(char *p, String *s, int dolower)
{
	s = s_reset(s);
	p = skipwhite(p);
	if(*p == '"'){
		p++;
		for(;*p && *p != '"'; p++)
			if(dolower)
				s_putc(s, tolower(*p));
			else
				s_putc(s, *p);
		if(*p == '"')
			p++;
		s_terminate(s);

		return p;
	}

	for(; *p && !isspace(*p) && *p != ';'; p++)
		if(dolower)
			s_putc(s, tolower(*p));
		else
			s_putc(s, *p);
	s_terminate(s);

	return p;
}

static char*
lowercase(char *p)
{
	char *op;
	int c;

	for(op = p; c = *p; p++)
		if(isupper(c))
			*p = tolower(c);
	return op;
}

/*
 *  return number of 8 bit characters
 */
static int
is8bit(Message *m)
{
	int count = 0;
	char *p;

	for(p = m->body; p < m->bend; p++)
		if(*p & 0x80)
			count++;
	return count;
}

// translate latin1 directly since it fits neatly in utf
int
latin1toutf(char *out, char *in, char *e)
{
	Rune r;
	char *p;

	p = out;
	for(; in < e; in++){
		r = (*in) & 0xff;
		p += runetochar(p, &r);
	}
	*p = 0;
	return p - out;
}

// translate any thing else using the tcs program
int
xtoutf(char *charset, char **out, char *in, char *e)
{
	char *av[4];
	int totcs[2];
	int fromtcs[2];
	int n, len, sofar;
	char *p;

	len = e-in+1;
	sofar = 0;
	*out = p = malloc(len+1);
	if(p == nil)
		return 0;

	av[0] = charset;
	av[1] = "-f";
	av[2] = charset;
	av[3] = 0;
	if(pipe(totcs) < 0)
		return 0;
	if(pipe(fromtcs) < 0){
		close(totcs[0]); close(totcs[1]);
		return 0;
	}
	switch(rfork(RFPROC|RFFDG|RFNOWAIT)){
	case -1:
		close(fromtcs[0]); close(fromtcs[1]);
		close(totcs[0]); close(totcs[1]);
		return 0;
	case 0:
		close(fromtcs[0]); close(totcs[1]);
		dup(fromtcs[1], 1);
		dup(totcs[0], 0);
		close(fromtcs[1]); close(totcs[0]);
		dup(open("/dev/null", OWRITE), 2);
		exec("/bin/tcs", av);
		_exits(0);
	default:
		close(fromtcs[1]); close(totcs[0]);
		switch(rfork(RFPROC|RFFDG|RFNOWAIT)){
		case -1:
			close(fromtcs[0]); close(totcs[1]);
			return 0;
		case 0:
			close(fromtcs[0]);
			while(in < e){
				n = write(totcs[1], in, e-in);
				if(n <= 0)
					break;
				in += n;
			}
			close(totcs[1]);
			_exits(0);
		default:
			close(totcs[1]);
			for(;;){
				n = read(fromtcs[0], &p[sofar], len-sofar);
				if(n <= 0)
					break;
				sofar += n;
				p[sofar] = 0;
				if(sofar == len){
					len += 1024;
					*out = p = realloc(p, len+1);
					if(p == nil)
						return 0;
				}
			}
			close(fromtcs[0]);
			break;
		}
		break;
	}
	return sofar;
}

enum {
	Winstart= 0x7f,
	Winend= 0x9f,
};

Rune winchars[] = {
	L'•',
	L'•', L'•', L'‚', L'ƒ', L'„', L'…', L'†', L'‡',
	L'ˆ', L'‰', L'Š', L'‹', L'Œ', L'•', L'•', L'•',
	L'•', L'‘', L'’', L'“', L'”', L'•', L'–', L'—',
	L'˜', L'™', L'š', L'›', L'œ', L'•', L'•', L'Ÿ',
};

int
windows1257toutf(char *out, char *in, char *e)
{
	Rune r;
	char *p;

	p = out;
	for(; in < e; in++){
		r = (*in) & 0xff;
		if(r >= 0x7f && r <= 0x9f)
			r = winchars[r-0x7f];
		p += runetochar(p, &r);
	}
	*p = 0;
	return p - out;
}
// convert latin1 to utf
void
convert(Message *m)
{
	int len;
	char *x;

	// don't convert if we're not a leaf, not text, or already converted
	if(m->converted)
		return;
	if(m->part != nil)
		return;
	if(cistrncmp(s_to_c(m->type), "text", 4) != 0)
		return;

	if(cistrcmp(s_to_c(m->charset), "us-ascii") == 0 ||
	   cistrcmp(s_to_c(m->charset), "iso-8859-1") == 0){
		len = is8bit(m);
		if(len > 0){
			len = 2*len + m->bend - m->body + 1;
			x = emalloc(len);
			len = latin1toutf(x, m->body, m->bend);
			if(m->ballocd)
				free(m->body);
			m->body = x;
			m->bend = x + len;
			m->ballocd = 1;
		}
	} else if(cistrcmp(s_to_c(m->charset), "iso-8859-2") == 0){
		len = xtoutf("8859-2", &x, m->body, m->bend);
		if(len != 0){
			if(m->ballocd)
				free(m->body);
			m->body = x;
			m->bend = x + len;
			m->ballocd = 1;
		}
	} else if(cistrcmp(s_to_c(m->charset), "iso-8859-15") == 0){
		len = xtoutf("8859-15", &x, m->body, m->bend);
		if(len != 0){
			if(m->ballocd)
				free(m->body);
			m->body = x;
			m->bend = x + len;
			m->ballocd = 1;
		}
	} else if(cistrcmp(s_to_c(m->charset), "big5") == 0){
		len = xtoutf("big5", &x, m->body, m->bend);
		if(len != 0){
			if(m->ballocd)
				free(m->body);
			m->body = x;
			m->bend = x + len;
			m->ballocd = 1;
		}
	} else if(cistrcmp(s_to_c(m->charset), "iso-2022-jp") == 0){
		len = xtoutf("jis", &x, m->body, m->bend);
		if(len != 0){
			if(m->ballocd)
				free(m->body);
			m->body = x;
			m->bend = x + len;
			m->ballocd = 1;
		}
	} else if(cistrcmp(s_to_c(m->charset), "windows-1257") == 0
			|| cistrcmp(s_to_c(m->charset), "windows-1252") == 0){
		len = is8bit(m);
		if(len > 0){
			len = 2*len + m->bend - m->body + 1;
			x = emalloc(len);
			len = windows1257toutf(x, m->body, m->bend);
			if(m->ballocd)
				free(m->body);
			m->body = x;
			m->bend = x + len;
			m->ballocd = 1;
		}
	} else if(cistrcmp(s_to_c(m->charset), "windows-1251") == 0){
		len = xtoutf("cp1251", &x, m->body, m->bend);
		if(len != 0){
			if(m->ballocd)
				free(m->body);
			m->body = x;
			m->bend = x + len;
			m->ballocd = 1;
		}
	} else if(cistrcmp(s_to_c(m->charset), "koi8-r") == 0){
		len = xtoutf("koi8", &x, m->body, m->bend);
		if(len != 0){
			if(m->ballocd)
				free(m->body);
			m->body = x;
			m->bend = x + len;
			m->ballocd = 1;
		}
	}

	m->converted = 1;
}

void *
erealloc(void *p, ulong n)
{
	if(n == 0)
		n = 1;
	p = realloc(p, n);
	if(!p){
		fprint(2, "%s: out of memory realloc %lud\n", argv0, n);
		exits("out of memory");
	}
	setrealloctag(p, getcallerpc(&p));
	return p;
}

void *
emalloc(ulong n)
{
	void *p;

	p = mallocz(n, 1);
	if(!p){
		fprint(2, "%s: out of memory alloc %lud\n", argv0, n);
		exits("out of memory");
	}
	setmalloctag(p, getcallerpc(&n));
	return p;
}

void
addtomessage(Message *m, uchar *p, int n, int done)
{
	int i, len;

	// add to message (+ 1 in malloc is for a trailing null)
	if(m->lim - m->end < n){
		if(m->start != nil){
			i = m->end-m->start;
			if(done)
				len = i + n;
			else
				len = (4*(i+n))/3;
			m->start = erealloc(m->start, len + 1);
			m->end = m->start + i;
		} else {
			if(done)
				len = n;
			else
				len = 2*n;
			m->start = emalloc(len + 1);
			m->end = m->start;
		}
		m->lim = m->start + len;
	}

	memmove(m->end, p, n);
	m->end += n;
}

int
readmessage(Message *m, Inbuf *inb)
{
	int i, n, done;
	uchar *p, *np;
	char sdigest[SHA1dlen*2+1];
	char tmp[64];

	for(done = 0; !done;){
		n = inb->wptr - inb->rptr;
		if(n < 6){
			if(n)
				memmove(inb->data, inb->rptr, n);
			inb->rptr = inb->data;
			inb->wptr = inb->rptr + n;
			i = read(inb->fd, inb->wptr, Buffersize);
			if(i < 0){
				if(fd2path(inb->fd, tmp, sizeof tmp) < 0)
					strcpy(tmp, "unknown mailbox");
				fprint(2, "error reading '%s': %r\n", tmp);
				return -1;
			}
			if(i == 0){
				if(n != 0)
					addtomessage(m, inb->rptr, n, 1);
				if(m->end == m->start)
					return -1;
				break;
			}
			inb->wptr += i;
		}

		// look for end of message
		for(p = inb->rptr; p < inb->wptr; p = np+1){
			// first part of search for '\nFrom '
			np = memchr(p, '\n', inb->wptr - p);
			if(np == nil){
				p = inb->wptr;
				break;
			}

			/*
			 *  if we've found a \n but there's
			 *  not enough room for '\nFrom ', don't do
			 *  the comparison till we've read in more.
			 */
			if(inb->wptr - np < 6){
				p = np;
				break;
			}

			if(strncmp((char*)np, "\nFrom ", 6) == 0){
				done = 1;
				p = np+1;
				break;
			}
		}

		// add to message (+ 1 in malloc is for a trailing null)
		n = p - inb->rptr;
		addtomessage(m, inb->rptr, n, done);
		inb->rptr += n;
	}

	// if it doesn't start with a 'From ', this ain't a mailbox
	if(strncmp(m->start, "From ", 5) != 0)
		return -1;

	// dump trailing newline, make sure there's a trailing null
	// (helps in body searches)
	if(*(m->end-1) == '\n')
		m->end--;
	*m->end = 0;
	m->bend = m->rbend = m->end;

	// digest message
	sha1((uchar*)m->start, m->end - m->start, m->digest, nil);
	for(i = 0; i < SHA1dlen; i++)
		sprint(sdigest+2*i, "%2.2ux", m->digest[i]);
	m->sdigest = s_copy(sdigest);
	return 0;
}

// delete a message from a mailbox
void
delmessage(Mailbox *mb, Message *m)
{
	Message **l;

	mb->vers++;

	if(m->whole != m){
		// unchain from parent
		for(l = &m->whole->part; *l && *l != m; l = &(*l)->next)
			;
		if(*l != nil)
			*l = m->next;
	}

	/* recurse through sub-parts */
	while(m->part)
		delmessage(mb, m->part);

	/* free memory */
	if(m->mallocd)
		free(m->start);
	if(m->hallocd)
		free(m->header);
	if(m->ballocd)
		free(m->body);
	s_free(m->unixfrom);
	s_free(m->unixdate);
	s_free(m->unixheader);
	s_free(m->from822);
	s_free(m->sender822);
	s_free(m->to822);
	s_free(m->bcc822);
	s_free(m->cc822);
	s_free(m->replyto822);
	s_free(m->date822);
	s_free(m->inreplyto822);
	s_free(m->subject822);
	s_free(m->messageid822);
	s_free(m->addrs);
	s_free(m->mimeversion);
	s_free(m->sdigest);
	s_free(m->boundary);
	s_free(m->type);
	s_free(m->charset);
	s_free(m->filename);

	free(m);
}

/*
 *  pick up a header line
 */
int
headerline(char **pp, String *hl)
{
	char *p, *x;

	s_reset(hl);
	p = *pp;
	x = strpbrk(p, ":\n");
	if(x == nil || *x == '\n')
		return 0;
	for(;;){
		x = strchr(p, '\n');
		if(x == nil)
			x = p + strlen(p);
		s_nappend(hl, p, x-p);
		p = x;
		if(*p != '\n' || *++p != ' ' && *p != '\t')
			break;
		while(*p == ' ' || *p == '\t')
			p++;
		s_putc(hl, ' ');
	}
	*pp = p;
	return 1;
}

String*
addr822(char *p)
{
	String *s, *list;
	int incomment, addrdone, inanticomment, quoted;
	int n;
	int c;

	list = s_new();
	s = s_new();
	quoted = incomment = addrdone = inanticomment = 0;
	n = 0;
	for(; *p; p++){
		c = *p;

		// whitespace is ignored
		if(!quoted && isspace(c) || c == '\r')
			continue;

		// strings are always treated as atoms
		if(!quoted && c == '"'){
			if(!addrdone && !incomment)
				s_putc(s, c);
			for(p++; *p; p++){
				if(!addrdone && !incomment)
					s_putc(s, *p);
				if(!quoted && *p == '"')
					break;
				if(*p == '\\')
					quoted = 1;
				else
					quoted = 0;
			}
			if(*p == 0)
				break;
			quoted = 0;
			continue;
		}

		// ignore everything in an expicit comment
		if(!quoted && c == '('){
			incomment = 1;
			continue;
		}
		if(incomment){
			if(!quoted && c == ')')
				incomment = 0;
			quoted = 0;
			continue;
		}

		// anticomments makes everything outside of them comments
		if(!quoted && c == '<' && !inanticomment){
			inanticomment = 1;
			s = s_reset(s);
			continue;
		}
		if(!quoted && c == '>' && inanticomment){
			addrdone = 1;
			inanticomment = 0;
			continue;
		}

		// commas separate addresses
		if(!quoted && c == ',' && !inanticomment){
			s_terminate(s);
			addrdone = 0;
			if(n++ != 0)
				s_append(list, " ");
			s_append(list, s_to_c(s));
			s = s_reset(s);
			continue;
		}

		// what's left is part of the address
		s_putc(s, c);

		// quoted characters are recognized only as characters
		if(c == '\\')
			quoted = 1;
		else
			quoted = 0;

	}

	if(*s_to_c(s) != 0){
		s_terminate(s);
		if(n++ != 0)
			s_append(list, " ");
		s_append(list, s_to_c(s));
	}
	s_free(s);

	if(n == 0){
		s_free(list);
		return nil;
	}
	return list;
}

void
to822(Message *m, Header *h, char *p)
{
	p += strlen(h->type);
	s_free(m->to822);
	m->to822 = addr822(p);
}

void
cc822(Message *m, Header *h, char *p)
{
	p += strlen(h->type);
	s_free(m->cc822);
	m->cc822 = addr822(p);
}

void
bcc822(Message *m, Header *h, char *p)
{
	p += strlen(h->type);
	s_free(m->bcc822);
	m->bcc822 = addr822(p);
}

void
from822(Message *m, Header *h, char *p)
{
	p += strlen(h->type);
	s_free(m->from822);
	m->from822 = addr822(p);
}

void
sender822(Message *m, Header *h, char *p)
{
	p += strlen(h->type);
	s_free(m->sender822);
	m->sender822 = addr822(p);
}

void
replyto822(Message *m, Header *h, char *p)
{
	p += strlen(h->type);
	s_free(m->replyto822);
	m->replyto822 = addr822(p);
}

void
mimeversion(Message *m, Header *h, char *p)
{
	p += strlen(h->type);
	s_free(m->mimeversion);
	m->mimeversion = addr822(p);
}

void
killtrailingwhite(char *p)
{
	char *e;

	e = p + strlen(p) - 1;
	while(e > p && isspace(*e))
		*e-- = 0;
}

void
date822(Message *m, Header *h, char *p)
{
	p += strlen(h->type);
	p = skipwhite(p);
	s_free(m->date822);
	m->date822 = s_copy(p);
	p = s_to_c(m->date822);
	killtrailingwhite(p);
}

void
subject822(Message *m, Header *h, char *p)
{
	p += strlen(h->type);
	p = skipwhite(p);
	s_free(m->subject822);
	m->subject822 = s_copy(p);
	p = s_to_c(m->subject822);
	killtrailingwhite(p);
}

void
inreplyto822(Message *m, Header *h, char *p)
{
	p += strlen(h->type);
	p = skipwhite(p);
	s_free(m->inreplyto822);
	m->inreplyto822 = s_copy(p);
	p = s_to_c(m->inreplyto822);
	killtrailingwhite(p);
}

void
messageid822(Message *m, Header *h, char *p)
{
	p += strlen(h->type);
	p = skipwhite(p);
	s_free(m->messageid822);
	m->messageid822 = s_copy(p);
	p = s_to_c(m->messageid822);
	killtrailingwhite(p);
}

int
isattribute(char **pp, char *attr)
{
	char *p;
	int n;

	n = strlen(attr);
	p = *pp;
	if(cistrncmp(p, attr, n) != 0)
		return 0;
	p += n;
	while(*p == ' ')
		p++;
	if(*p++ != '=')
		return 0;
	while(*p == ' ')
		p++;
	*pp = p;
	return 1;
}
static void
setfilename(Message *m, char *p)
{
	m->filename = s_reset(m->filename);
	getstring(p, m->filename, 0);
	for(p = s_to_c(m->filename); *p; p++)
		if(*p == ' ' || *p == '\t' || *p == ';')
			*p = '_';
}

void
ctype(Message *m, Header *h, char *p)
{
	String *s;

	p += h->len;
	p = skipwhite(p);

	p = getstring(p, m->type, 1);
	
	while(*p){
		if(isattribute(&p, "boundary")){
			s = s_new();
			p = getstring(p, s, 0);
			m->boundary = s_reset(m->boundary);
			s_append(m->boundary, "--");
			s_append(m->boundary, s_to_c(s));
			s_free(s);
		} else if(cistrncmp(p, "multipart", 9) == 0){
			/*
			 *  the first unbounded part of a multipart message,
			 *  the preamble, is not displayed or saved
			 */
		} else if(isattribute(&p, "name")){
			if(m->filename == nil)
				setfilename(m, p);
		} else if(isattribute(&p, "charset")){
			p = getstring(p, s_reset(m->charset), 0);
		}
		
		p = skiptosemi(p);
	}
}

void
cencoding(Message *m, Header *h, char *p)
{
	p += h->len;
	p = skipwhite(p);
	if(cistrncmp(p, "base64", 6) == 0)
		m->encoding = Ebase64;
	else if(cistrncmp(p, "quoted-printable", 16) == 0)
		m->encoding = Equoted;
}

void
cdisposition(Message *m, Header *h, char *p)
{
	p += h->len;
	p = skipwhite(p);
	while(*p){
		if(cistrncmp(p, "inline", 6) == 0){
			m->disposition = Dinline;
		} else if(cistrncmp(p, "attachment", 10) == 0){
			m->disposition = Dfile;
		} else if(cistrncmp(p, "filename=", 9) == 0){
			p += 9;
			setfilename(m, p);
		}
		p = skiptosemi(p);
	}

}

/*
 *  parse a Unix style header
 */
void
parseunix(Message *m)
{
	char *p;
	String *h;

	h = s_new();
	for(p = m->start + 5; *p && *p != '\r' && *p != '\n'; p++)
		s_putc(h, *p);
	s_terminate(h);
	s_restart(h);

	m->unixfrom = s_parse(h, s_reset(m->unixfrom));
	m->unixdate = s_append(s_reset(m->unixdate), h->ptr);

	s_free(h);
}
/*
 *  squeeze nulls out of the body
 */
static void
nullsqueeze(Message *m)
{
	char *p, *q;

	q = memchr(m->body, 0, m->end-m->body);
	if(q == nil)
		return;

	for(p = m->body; q < m->end; q++){
		if(*q == 0)
			continue;
		*p++ = *q;
	}
	m->bend = m->rbend = m->end = p;
}

extern int strtotm(char*, Tm*);

String*
date822tounix(char *s)
{
	char *p, *q;
	Tm tm;

	if(strtotm(s, &tm) < 0)
		return nil;

	p = asctime(&tm);
	if(q = strchr(p, '\n'))
		*q = '\0';
	return s_copy(p);
}


/*
 *  parse a message
 */
void
parseheaders(Message *m, int justmime, Mailbox *, int addfrom)
{
	String *hl;
	Header *h;
	char *p, *q;


	// parse mime headers
	p = m->header;
	hl = s_new();
	while(headerline(&p, hl)){
		if(justmime)
			h = &head[Mhead];
		else
			h = head;
		for(; h->type; h++){
			if(cistrncmp(s_to_c(hl), h->type, h->len) == 0){
				(*h->f)(m, h, s_to_c(hl));
				break;
			}
		}
		s_reset(hl);
	}
	s_free(hl);

	// the blank line isn't really part of the body or header
	if(justmime){
		m->mhend = p;
		m->hend = m->header;
	} else {
		m->hend = p;
	}
	if(*p == '\n')
		p++;
	m->rbody = m->body = p;

	// if type is text, get any nulls out of the body.  This is
	// for the two seans and imap clients that get confused.
	if(strncmp(s_to_c(m->type), "text/", 5) == 0)
		nullsqueeze(m);

	//
	// cobble together Unix-style from line
	// for local mailbox messages, we end up recreating the
	// original header.
	// for pop3 messages, the best we can do is 
	// use the From: information and the RFC822 date.
	//
	if(m->unixdate == nil || strcmp(s_to_c(m->unixdate), "???") == 0
	|| strcmp(s_to_c(m->unixdate), "Thu Jan 1 00:00:00 GMT 1970") == 0){
		if(m->unixdate){
			s_free(m->unixdate);
			m->unixdate = nil;
		}
		// look for the date in the first Received: line.
		// it's likely to be the right time zone (it's
	 	// the local system) and in a convenient format.
		if(cistrncmp(m->header, "received:", 9)==0){
			if((q = strchr(m->header, ';')) != nil){
				p = q;
				while((p = strchr(p, '\n')) != nil){
					if(p[1] != ' ' && p[1] != '\t' && p[1] != '\n')
						break;
					p++;
				}
				if(p){
					*p = '\0';
					m->unixdate = date822tounix(q+1);
					*p = '\n';
				}
			}
		}

		// fall back on the rfc822 date	
		if(m->unixdate==nil && m->date822)
			m->unixdate = date822tounix(s_to_c(m->date822));
	}

	if(m->unixheader != nil)
		s_free(m->unixheader);

	// only fake header for top-level messages for pop3 and imap4
	// clients (those protocols don't include the unix header).
	// adding the unix header all the time screws up mime-attached
	// rfc822 messages.
	if(!addfrom && !m->unixfrom){
		m->unixheader = nil;
		return;
	}

	m->unixheader = s_copy("From ");
	if(m->unixfrom && strcmp(s_to_c(m->unixfrom), "???") != 0)
		s_append(m->unixheader, s_to_c(m->unixfrom));
	else if(m->from822)
		s_append(m->unixheader, s_to_c(m->from822));
	else
		s_append(m->unixheader, "???");

	s_append(m->unixheader, " ");
	if(m->unixdate)
		s_append(m->unixheader, s_to_c(m->unixdate));
	else
		s_append(m->unixheader, "Thu Jan  1 00:00:00 GMT 1970");

	s_append(m->unixheader, "\n");
}

String*
promote(String **sp)
{
	String *s;

	if(*sp != nil)
		s = s_clone(*sp);
	else
		s = nil;
	return s;
}

enum
{
	Self=	1,
	Hex=	2,
};
uchar	tableqp[256];

static void
initquoted(void)
{
	int c;

	memset(tableqp, 0, 256);
	for(c = ' '; c <= '<'; c++)
		tableqp[c] = Self;
	for(c = '>'; c <= '~'; c++)
		tableqp[c] = Self;
	tableqp['\t'] = Self;
	tableqp['='] = Hex;
}

static int
hex2int(int x)
{
	if(x >= '0' && x <= '9')
		return x - '0';
	if(x >= 'A' && x <= 'F')
		return (x - 'A') + 10;
	if(x >= 'a' && x <= 'f')
		return (x - 'a') + 10;
	return 0;
}

static char*
decquotedline(char *out, char *in, char *e)
{
	int c, soft;

	/* dump trailing white space */
	while(e >= in && (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n'))
		e--;

	/* trailing '=' means no newline */
	if(*e == '='){
		soft = 1;
		e--;
	} else
		soft = 0;

	while(in <= e){
		c = (*in++) & 0xff;
		switch(tableqp[c]){
		case Self:
			*out++ = c;
			break;
		case Hex:
			c = hex2int(*in++)<<4;
			c |= hex2int(*in++);
			*out++ = c;
			break;
		}
	}
	if(!soft)
		*out++ = '\n';
	*out = 0;

	return out;
}

int
decquoted(char *out, char *in, char *e)
{
	char *p, *nl;

	if(tableqp[' '] == 0)
		initquoted();

	p = out;
	while((nl = strchr(in, '\n')) != nil && nl < e){
		p = decquotedline(p, in, nl);
		in = nl + 1;
	}
	if(in < e)
		p = decquotedline(p, in, e-1);

	// make sure we end with a new line
	if(*(p-1) != '\n'){
		*p++ = '\n';
		*p = 0;
	}

	return p - out;
}

//
// decode message body
//
void
decode(Message *m)
{
	int i, len;
	char *x;

	if(m->decoded)
		return;
	switch(m->encoding){
	case Ebase64:
		len = m->bend - m->body;
		i = (len*3)/4+1;	// room for max chars + null
		x = emalloc(i);
		len = dec64((uchar*)x, i, m->body, len);
		if(m->ballocd)
			free(m->body);
		m->body = x;
		m->bend = x + len;
		m->ballocd = 1;
		break;
	case Equoted:
		len = m->bend - m->body;
		x = emalloc(len+2);	// room for null and possible extra nl
		len = decquoted(x, m->body, m->bend);
		if(m->ballocd)
			free(m->body);
		m->body = x;
		m->bend = x + len;
		m->ballocd = 1;
		break;
	default:
		break;
	}
	m->decoded = 1;
}

void
parsebody(Message *m, Mailbox *mb)
{
	Message *nm;

	// recurse
	if(strncmp(s_to_c(m->type), "multipart/", 10) == 0){
		parseattachments(m, mb);
	} else if(strcmp(s_to_c(m->type), "message/rfc822") == 0){
		decode(m);
		parseattachments(m, mb);
		nm = m->part;

		// promote headers
		if(m->replyto822 == nil && m->from822 == nil && m->sender822 == nil){
			m->from822 = promote(&nm->from822);
			m->to822 = promote(&nm->to822);
			m->date822 = promote(&nm->date822);
			m->sender822 = promote(&nm->sender822);
			m->replyto822 = promote(&nm->replyto822);
			m->subject822 = promote(&nm->subject822);
			m->unixdate = promote(&nm->unixdate);
		}
		convert(m);
	}
}

void
digest(Message* m)
{
	char sdigest[SHA1dlen*2+1];
	DigestState*	d;
	char*	s;
	int	i;

	s = " ";
	if (m->subject822 && s_to_c(m->subject822))
		s = s_to_c(m->subject822);
	d = sha1((uchar*)s, strlen(s), nil, nil);
	if (m->body && m->bend > m->body)
		sha1((uchar*)m->body, m->bend - m->body, m->digest, d);
	else
		sha1((uchar*)" ", 1, m->digest, d);
	for(i = 0; i < SHA1dlen; i++)
		sprint(sdigest+2*i, "%2.2ux", m->digest[i]);
	m->sdigest = s_copy(sdigest);
}

void
parse(Message *m, int justmime, Mailbox *mb, int addfrom)
{
	parseheaders(m, justmime, mb, addfrom);
	parsebody(m, mb);
	decode(m);
	convert(m);
	digest(m);
}

Message*
newmessage(Message *parent)
{
	static int id;
	Message *m;

	m = emalloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->disposition = Dnone;
	m->type = s_copy("text/plain");
	m->charset = s_copy("iso-8859-1");
	m->id = id++;
	if(parent)
		sprint(m->name, "%d", ++(parent->subname));
	if(parent == nil)
		parent = m;
	m->whole = parent;
	m->hlen = -1;
	return m;
}

void
parseattachments(Message *m, Mailbox *mb)
{
	Message *nm, **l;
	char *p, *x;

	// if there's a boundary, recurse...
	if(m->boundary != nil){
		p = m->body;
		nm = nil;
		l = &m->part;
		for(;;){
			x = strstr(p, s_to_c(m->boundary));

			/* no boundary, we're done */
			if(x == nil){
				if(nm != nil)
					nm->rbend = nm->bend = nm->end = m->bend;
				break;
			}

			/* boundary must be at the start of a line */
			if(x != m->body && *(x-1) != '\n'){
				p = x+1;
				continue;
			}

			if(nm != nil)
				nm->rbend = nm->bend = nm->end = x;
			x += strlen(s_to_c(m->boundary));

			/* is this the last part? ignore anything after it */
			if(strncmp(x, "--", 2) == 0)
				break;

			p = strchr(x, '\n');
			if(p == nil)
				break;
			nm = newmessage(m);
			nm->start = nm->header = nm->body = nm->rbody = ++p;
			nm->mheader = nm->header;
			*l = nm;
			l = &nm->next;
		}
		for(nm = m->part; nm != nil; nm = nm->next)
			parse(nm, 1, mb, 0);
		return;
	}

	// if we've got an rfc822 message, recurse...
	if(strcmp(s_to_c(m->type), "message/rfc822") == 0){
		nm = newmessage(m);
		m->part = nm;
		nm->start = nm->header = nm->body = nm->rbody = m->body;
		nm->end = nm->bend = nm->rbend = m->bend;
		parse(nm, 0, mb, 0);
	}
}

char*
readmbox(Mailbox *mb)
{
	int fd;
	String *tmp;
	Dir *d;
	static char err[128];
	Message *m, **l;
	Inbuf *inb;
	char *x;

	initheaders();
	l = &mb->root->part;

	/*
	 *  open the mailbox.  If it doesn't exist, try the temporary one.
	 */
retry:
	dprint(2, "opening %s\n", mb->path);
	fd = open(mb->path, OREAD);
	if(fd < 0){
		errstr(err, sizeof(err));
		if(strstr(err, "exist") != 0){
			tmp = s_copy(mb->path);
			s_append(tmp, ".tmp");
			if(sysrename(s_to_c(tmp), mb->path) == 0){
				s_free(tmp);
				goto retry;
			}
			s_free(tmp);
		}
		return err;
	}

	d = dirfstat(fd);
	if(d == nil){
		close(fd);
		errstr(err, sizeof(err));
		return err;
	}
	mb->d = d;
	mb->vers++;

	inb = emalloc(sizeof(Inbuf));
	inb->rptr = inb->wptr = inb->data;
	inb->fd = fd;

	//  read new messages
	snprint(err, sizeof err, "reading '%s'", mb->path);
	for(;;){
		m = newmessage(mb->root);
		m->mallocd = 1;
		m->inmbox = 1;
		if(readmessage(m, inb) < 0){
			delmessage(mb, m);
			mb->root->subname--;
			break;
		}
		dprint(2, "readmessage %d\n", m->id);

		// merge mailbox versions
		while(*l != nil){
			if(memcmp((*l)->digest, m->digest, SHA1dlen) == 0){
				// matches mail we already read, discard
				delmessage(mb, m);
				mb->root->subname--;
				m = nil;
				l = &(*l)->next;
				break;
			} else {
				(*l)->inmbox = 0;
				(*l)->deleted = 1;
				l = &(*l)->next;
			}
		}
		if(m == nil)
			continue;

		x = strchr(m->start, '\n');
		if(x == nil)
			m->header = m->end;
		else
			m->header = x + 1;
		m->mheader = m->mhend = m->header;
		parseunix(m);
		parse(m, 0, mb, 0);

		/* chain in */
		*l = m;
		l = &m->next;

	}


	close(fd);
	free(inb);
	return nil;
}

/* create a new mailbox */
Mailbox*
newmbox(char *path, char *name, int std)
{
	Mailbox *mb;
	char *p, *rv;


	mb = emalloc(sizeof(*mb));
	strncpy(mb->path, path, sizeof(mb->path)-1);
	if(name == nil){
		p = strrchr(path, '/');
		if(p == nil)
			p = path;
		else
			p++;
		if(*p == 0){
			free(mb);
			return nil;
		}
		strncpy(mb->name, p, sizeof(mb->name)-1);
	} else {
		strncpy(mb->name, name, sizeof(mb->name)-1);
	}

	rv = nil;

	// on error, give up
	if(rv){
		free(mb);
		return nil;
	}


	mb->refs = 1;
	mb->next = nil;
	mb->id = 0;
	mb->root = newmessage(nil);
	mb->std = std;

	return mb;
}
