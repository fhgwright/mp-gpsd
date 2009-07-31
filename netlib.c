/* $Id$ */
#include <sys/types.h>
#include "gpsd_config.h"
#ifdef HAVE_SYS_SOCKET_H
#ifndef S_SPLINT_S
#include <sys/socket.h>
#endif /* S_SPLINT_S */
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_NETINET_IN_SYSTM_H
#include <netinet/in_systm.h>
#endif
#ifndef S_SPLINT_S
#ifdef HAVE_NETINET_IN_H
#include <netinet/ip.h>
#endif /* S_SPLINT_S */
#endif
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "gpsd.h"

#if !defined (INADDR_NONE)
#define INADDR_NONE   ((in_addr_t)-1)
#endif

int netlib_connectsock(const char *host, const char *service, const char *protocol)
{
    struct hostent *phe;
    struct servent *pse;
    struct protoent *ppe;
    struct sockaddr_in sin;
    int s, type, proto, one = 1;

    memset((char *) &sin, 0, sizeof(sin));
    /*@ -type -mustfreefresh @*/
    sin.sin_family = AF_INET;
    if ((pse = getservbyname(service, protocol)))
	sin.sin_port = htons(ntohs((unsigned short) pse->s_port));
    else if ((sin.sin_port = htons((unsigned short) atoi(service))) == 0)
	return NL_NOSERVICE;

    if ((phe = gethostbyname(host)))
	memcpy((char *) &sin.sin_addr, phe->h_addr, phe->h_length);
    else if ((sin.sin_addr.s_addr = inet_addr(host)) == INADDR_NONE)
	return NL_NOHOST;

    ppe = getprotobyname(protocol);
    if (strcmp(protocol, "udp") == 0) {
	type = SOCK_DGRAM;
	proto = (ppe) ? ppe->p_proto : IPPROTO_UDP;
    } else {
	type = SOCK_STREAM;
	proto = (ppe) ? ppe->p_proto : IPPROTO_TCP;
    }

    if ((s = socket(PF_INET, type, proto)) < 0)
	return NL_NOSOCK;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one))==-1) {
	(void)close(s);
	return NL_NOSOCKOPT;
    }
    if (connect(s, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
	(void)close(s);
	return NL_NOCONNECT;
    }

#ifdef IPTOS_LOWDELAY
    {
    int opt = IPTOS_LOWDELAY;
    /*@ -unrecog @*/
    (void)setsockopt(s, IPPROTO_IP, IP_TOS, &opt, sizeof opt);
    /*@ +unrecog @*/
    }
#endif
#ifdef TCP_NODELAY
    if (type == SOCK_STREAM)
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#endif
    return s;
    /*@ +type +mustfreefresh @*/
}

char *sock2ip(int fd)
{
    struct sockaddr fsin;
    socklen_t alen = (socklen_t)sizeof(fsin);
    char *ip;
    int r;

    r = getpeername(fd, (struct sockaddr *) &fsin, &alen);
    /*@ -branchstate @*/
    if (r == 0){
	ip = inet_ntoa(((struct sockaddr_in *)(&fsin))->sin_addr);
    } else {
	gpsd_report(LOG_INF, "getpeername() = %d, error = %s (%d)\n", r, strerror(errno), errno);
	ip = "<unknown>";
    }
    /*@ +branchstate @*/
    return ip;
}
