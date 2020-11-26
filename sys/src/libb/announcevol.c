#include <u.h>
#include <libc.h>
#include <b.h>

static char*
cleanvoladdr(char* addr)
{
	char	naddr[50];
	char*	p;

	p = strchr(addr, '*');
	if (p == nil)
		return strdup(addr);
	else {
		if (p - addr)
			strncpy(naddr, addr, p - addr);
		naddr[p-addr] = 0;
		strcat(naddr, sysname());
		strcat(naddr, p + 1);
		return strdup(naddr);
	}
}

int
announcevol(int afd, char* addr, char* name, char* cnstr)
{
	int	sfd;
	char*	cfg;
	char*	lcfg;
	char*	loc;
	char*	locc;
	char*	saddr;
	char*	fs;
	char*	port;
	addr = cleanvoladdr(addr);
	port = strrchr(addr, '!');
	assert(port);
	port++;
	if (cnstr == nil) {
		loc = getenv("location");
		if (loc == nil)
			locc = strdup("");
		else
			locc = smprint(" loc=%s", loc);
		cfg = smprint("%s\t-\t/\t%s\t'user=%s sys=%s%s'",
			addr, name, getuser(), sysname(), locc);
		lcfg = smprint("tcp!127.0.0.1!%s\t-\t/\t%s\t'user=%s sys=%s%s'",
			port, name, getuser(), sysname(), locc);
		free(loc);
		free(locc);
	} else {
		lcfg = smprint("tcp!127.0.0.1!%s\t-\t/\t%s\t'%s'", port, name, cnstr);
		cfg = smprint("%s\t-\t/\t%s\t'%s'", addr, name, cnstr);
	}
	fs = getenv("fs");
	if (fs == nil){
		fs = getenv("fileserver");
		if (fs == nil)
			fs = strdup("whale");
	}
	saddr = smprint("tcp!%s!11010", fs);
	free(fs);
	if (afd < 0){
		sfd = open("/dev/vol", OWRITE);
		if (sfd >= 0){
			fprint(sfd, "%s\n", lcfg);
			close(sfd);
		}
		afd = dial(saddr, nil, nil, nil);
	}
	free(saddr);
	if (afd >= 0 && fprint(afd, "PlanB announce:\n%s\n", cfg) < 0){
			close(afd);
			afd = -1;
	}
	free(cfg);
	free(lcfg);
	return afd;
}
