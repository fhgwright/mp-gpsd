#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <assert.h>
#include <pwd.h>
#include <stdbool.h>
#include <math.h>

#include "config.h"
#if defined (HAVE_PATH_H)
#include <paths.h>
#else
#if !defined (_PATH_DEVNULL)
#define _PATH_DEVNULL    "/dev/null"
#endif
#endif
#if defined (HAVE_SYS_SELECT_H)
#include <sys/select.h>
#endif
#if defined (HAVE_SYS_STAT_H)
#include <sys/stat.h>
#endif
#if defined(HAVE_SYS_TIME_H)
#include <sys/time.h>
#endif

#if DBUS_ENABLE
#include <gpsd_dbus.h>
#endif

#include "gpsd.h"
#include "timebase.h"

/*
 * Timeout policy.  We can't rely on clients closing connections 
 * correctly, so we need timeouts to tell us when it's OK to 
 * reclaim client fds.  The assignment timeout fends off programs
 * that open connections and just sit there, not issuing a W or
 * doing anything else that triggers a device assignment.  Clients
 * in watcher or raw mode that don't read their data will get dropped 
 * when throttled_write() fills up the outbound buffers and the 
 * NOREAD_TIMEOUT expires.  Clients in the original polling mode have 
 * to be timed out as well.
 */
#define ASSIGNMENT_TIMEOUT	60
#define POLLER_TIMEOUT  	60*15
#define NOREAD_TIMEOUT		60*3

#define QLEN			5

/* Where to find the list of DGPS correction servers, if there is one */
#define DGPS_SERVER_LIST	"/usr/share/gpsd/dgpsip-servers"


/*
 * The name of a tty device from which to pick up whatever the local
 * owning group for tty devices is.  Used when we drop privileges.
 */
#define PROTO_TTY "/dev/ttyS0"

static fd_set all_fds;
static int debuglevel;
static bool in_background = false;
static jmp_buf restartbuf;
/*@ -initallelements -nullassign -nullderef @*/
static struct gps_context_t context = {
    .valid        = 0, 
    .sentdgps     = false, 
    .fixcnt       = 0, 
    .dsock        = -1, 
    .rtcmbytes    = 0, 
    .rtcmbuf      = {'\0'}, 
    .rtcmtime     = 0,
    .leap_seconds = LEAP_SECONDS, 
    .century      = CENTURY_BASE, 
#ifdef NTPSHM_ENABLE
    .shmTime      = {0},
    .shmTimeInuse = {0},
# ifdef PPS_ENABLE
    .shmTimePPS   = false,
# endif /* PPS_ENABLE */
#endif /* NTPSHM_ENABLE */
};
/*@ +initallelements +nullassign +nullderef @*/

static void onsig(int sig)
{
    longjmp(restartbuf, sig+1);
}

static int daemonize(void)
{
    int fd;
    pid_t pid;

    switch (pid = fork()) {
    case -1:
	return -1;
    case 0:	/* child side */
	break;
    default:	/* parent side */
	exit(0);
    }

    if (setsid() == -1)
	return -1;
    (void)chdir("/");
    /*@ -nullpass @*/
    if ((fd = open(_PATH_DEVNULL, O_RDWR, 0)) != -1) {
	(void)dup2(fd, STDIN_FILENO);
	(void)dup2(fd, STDOUT_FILENO);
	(void)dup2(fd, STDERR_FILENO);
	if (fd > 2)
	    (void)close(fd);
    }
    /*@ +nullpass @*/
    in_background = true;
    return 0;
}

#if defined(PPS_ENABLE)
static pthread_mutex_t report_mutex;
#endif /* PPS_ENABLE */

void gpsd_report(int errlevel, const char *fmt, ... )
/* assemble command in printf(3) style, use stderr or syslog */
{
    if (errlevel <= debuglevel) {
	char buf[BUFSIZ], buf2[BUFSIZ], *sp;
	va_list ap;

#if defined(PPS_ENABLE)
	(void)pthread_mutex_lock(&report_mutex);
#endif /* PPS_ENABLE */
	(void)strcpy(buf, "gpsd: ");
	va_start(ap, fmt) ;
	(void)vsnprintf(buf + strlen(buf), sizeof(buf)-strlen(buf), fmt, ap);
	va_end(ap);

	buf2[0] = '\0';
	for (sp = buf; *sp != '\0'; sp++)
	    if (isprint(*sp) || (isspace(*sp) && (sp[1]=='\0' || sp[2]=='\0')))
		(void)snprintf(buf2+strlen(buf2), 2, "%c", *sp);
	    else
		(void)snprintf(buf2+strlen(buf2), 6, "\\x%02x", (unsigned)*sp);

	if (in_background)
	    syslog((errlevel == 0) ? LOG_ERR : LOG_NOTICE, "%s", buf2);
	else
	    (void)fputs(buf2, stderr);
#if defined(PPS_ENABLE)
	(void)pthread_mutex_unlock(&report_mutex);
#endif /* PPS_ENABLE */
    }
}

static void usage(void)
{
    (void)printf("usage: gpsd [-d dgpsip-server] [-D n] [-F sockfile] [-P pidfile] [-S port] [-h] device...\n\
  Options include: \n\
  -d host[:port]         	= set DGPS server \n\
  -F sockfile                   = specift control socket location\n\
  -P pidfile              	= set file to record process ID \n\
  -D integer (default 0)  	= set debug level \n\
  -S integer (default %s)	= set port for daemon \n\
  -h                     	= help message \n",
	   DEFAULT_GPSD_PORT);
}

static bool have_fix(struct gps_device_t *device)
{
    if (!device) {
	gpsd_report(4, "Client has no device\n");
	return false;
    }
#define VALIDATION_COMPLAINT(level, legend) \
	gpsd_report(level, legend " (status=%d, mode=%d).\n", \
		    device->gpsdata.status, device->gpsdata.fix.mode)
    if ((device->gpsdata.status == STATUS_NO_FIX) != (device->gpsdata.fix.mode == MODE_NO_FIX)) {
	VALIDATION_COMPLAINT(3, "GPS is confused about whether it has a fix");
	return false;
    }
    else if (device->gpsdata.status > STATUS_NO_FIX && device->gpsdata.fix.mode != MODE_NO_FIX) {
	VALIDATION_COMPLAINT(3, "GPS has a fix");
	return true;
    }
    VALIDATION_COMPLAINT(3, "GPS has no fix");
    return false;
#undef VALIDATION_CONSTRAINT
}

static int passivesock(char *service, char *protocol, int qlen)
{
    struct servent *pse;
    struct protoent *ppe ;
    struct sockaddr_in sin;
    int s, type, one = 1;

    /*@ -mustfreefresh @*/
    memset((char *) &sin, 0, sizeof(sin));
    /*@i1@*/sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;

    if ((pse = getservbyname(service, protocol)))
	sin.sin_port = htons(ntohs((in_port_t)pse->s_port));
    else if ((sin.sin_port = htons((in_port_t)atoi(service))) == 0) {
	gpsd_report(0, "Can't get \"%s\" service entry.\n", service);
	return -1;
    }
    if ((ppe = getprotobyname(protocol)) == NULL) {
	gpsd_report(0, "Can't get \"%s\" protocol entry.\n", protocol);
	return -1;
    }
    if (strcmp(protocol, "udp") == 0)
	type = SOCK_DGRAM;
    else
	type = SOCK_STREAM;
    if ((s = socket(PF_INET, type, /*@i1@*/ppe->p_proto)) < 0) {
	gpsd_report(0, "Can't create socket\n");
	return -1;
    }
    if (setsockopt(s,SOL_SOCKET,SO_REUSEADDR,(char *)&one,(int)sizeof(one)) == -1) {
	gpsd_report(0, "Error: SETSOCKOPT SO_REUSEADDR\n");
	return -1;
    }
    if (bind(s, (struct sockaddr *) &sin, (int)sizeof(sin)) < 0) {
	gpsd_report(0, "Can't bind to port %s\n", service);
	return -1;
    }
    if (type == SOCK_STREAM && listen(s, qlen) < 0) {
	gpsd_report(0, "Can't listen on %s port%s\n", service);
	return -1;
    }
    return s;
    /*@ +mustfreefresh @*/
}

static int filesock(char *filename)
{
    struct sockaddr_un addr;
    int sock;

    /*@ -mayaliasunique @*/
    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
	gpsd_report(0, "Can't create device-control socket\n");
	return -1;
    }
    (void)strcpy(addr.sun_path, filename);
    /*@i1@*/addr.sun_family = AF_UNIX;
    (void)bind(sock, (struct sockaddr *) &addr, 
	 (int)(strlen(addr.sun_path) + sizeof (addr.sun_family)));
    if (listen(sock, QLEN) < 0) {
	gpsd_report(0, "can't listen on local socket %s\n", filename);
	return -1;
    }
    /*@ +mayaliasunique @*/
    return sock;
}

/*
 * Multi-session support requires us to have two arrays, one of GPS 
 * devices currently available and one of client sessions.  The number
 * of slots in each array is limited by the maximum number of client
 * sessions we can have open.
 */
#define MAXDEVICES	FD_SETSIZE

static struct gps_device_t channels[MAXDEVICES];
#define allocated_channel(chp)	((chp)->gpsdata.gps_device[0] != '\0')
#define free_channel(chp)	(chp)->gpsdata.gps_device[0] = '\0'
#define syncing(chp)	(chp->gpsdata.gps_fd>-1&& chp->packet_type==BAD_PACKET)

static struct subscriber_t {
    double active;			/* when subscriber signed on */
    bool tied;				/* client set device with F */
    bool watcher;			/* is client in watcher mode? */
    int raw;				/* is client in raw mode? */
    enum {GPS,RTCM104,ANY} requires;	/* type of device requested */
    /*@relnull@*/struct gps_device_t *device;	/* device subscriber listens to */
} subscribers[FD_SETSIZE];		/* indexed by client file descriptor */

static void detach_client(int cfd)
{
    (void)close(cfd);
    gpsd_report(4, "detaching %d in detach_client\n", cfd);
    FD_CLR(cfd, &all_fds);
    subscribers[cfd].raw = 0;
    subscribers[cfd].watcher = false;
    subscribers[cfd].active = 0;
    subscribers[cfd].device = NULL;
}

static ssize_t throttled_write(int cfd, char *buf, ssize_t len)
/* write to client -- throttle if it's gone or we're close to buffer overrun */
{
    ssize_t status;

    if (debuglevel >= 3) {
	if (isprint(buf[0]))
	    gpsd_report(3, "=> client(%d): %s", cfd, buf);
	else {
	    char *cp, buf2[MAX_PACKET_LENGTH*3];
	    buf2[0] = '\0';
	    for (cp = buf; cp < buf + len; cp++)
		(void)snprintf(buf2 + strlen(buf2), 
			       sizeof(buf2)-strlen(buf2),
			      "%02x", (unsigned int)(*cp & 0xff));
	    gpsd_report(3, "=> client(%d): =%s\r\n", cfd, buf2);
	}
    }

    if ((status = write(cfd, buf, (size_t)len)) > -1)
	return status;
    if (errno == EBADF)
	gpsd_report(3, "client(%d) has vanished.\n", cfd);
    else if (errno == EWOULDBLOCK && timestamp() - subscribers[cfd].active > NOREAD_TIMEOUT)
	gpsd_report(3, "client(%d) timed out.\n", cfd);
    else
	gpsd_report(3, "client(%d) write: %s\n", cfd, strerror(errno));
    detach_client(cfd);
    return status;
}

static void notify_watchers(struct gps_device_t *device, char *sentence, ...)
/* notify all clients watching a given device of an event */
{
    int cfd;
    va_list ap;
    char buf[BUFSIZ];

    va_start(ap, sentence) ;
    (void)vsnprintf(buf, sizeof(buf), sentence, ap);
    va_end(ap);

    for (cfd = 0; cfd < FD_SETSIZE; cfd++)
	if (subscribers[cfd].watcher != 0 && subscribers[cfd].device == device)
	    (void)throttled_write(cfd, buf, (ssize_t)strlen(buf));
}

static void raw_hook(struct gps_data_t *ud, 
		     char *sentence, size_t len, int level)
/* hook to be executed on each incoming packet */
{
    int cfd;

    for (cfd = 0; cfd < FD_SETSIZE; cfd++) {
	/* copy raw NMEA sentences from GPS to clients in raw mode */
	if (subscribers[cfd].raw == level && 
	    subscribers[cfd].device!=NULL &&
	    strcmp(ud->gps_device, subscribers[cfd].device->gpsdata.gps_device)==0)
	    (void)throttled_write(cfd, sentence, (ssize_t)len);
    }
}

/*@ -globstate @*/
static /*@null@*/ /*@observer@*/struct gps_device_t *find_device(char *device_name)
/* find the channel block for an existing device name */
{
    struct gps_device_t *chp;

    for (chp = channels; chp < channels + MAXDEVICES; chp++)
	if (allocated_channel(chp) && strcmp(chp->gpsdata.gps_device, device_name)==0)
	    return chp;
    return NULL;
}

static /*@null@*/ struct gps_device_t *open_device(char *device_name)
/* open and initialize a new channel block */
{
    struct gps_device_t *chp;

    for (chp = channels; chp < channels + MAXDEVICES; chp++)
	if (!allocated_channel(chp)){
            chp->saved_baud = -1;
	    goto found;
        }
    return NULL;
found:
    gpsd_init(chp, &context, device_name);
    chp->gpsdata.raw_hook = raw_hook;
    if (gpsd_activate(chp) < 0) {
	return NULL;
    }
    gpsd_report(4, "flagging descriptor %d in open_device\n", chp->gpsdata.gps_fd);
    FD_SET(chp->gpsdata.gps_fd, &all_fds);
    return chp;
}
/*@ +globstate @*/

static bool allocation_policy(struct gps_device_t *channel,
			      struct subscriber_t *user,
			      double most_recent)
{
#ifdef __UNUSED__
    /* only allocate devices that we know the packet type of */
    if (channel->packet_type == BAD_PACKET)
	return false;
#endif /* __UNUSED__ */
    /* maybe we have already bound a more recently active device */
    if (user->device!=NULL && channel->gpsdata.sentence_time < most_recent)
	return false;
    gpsd_report(1, "User requires %d, channel type is %d\n", user->requires, channel->packet_type);
    /* we might have type constraints */
    if (user->requires == ANY)
	return true;
    else if (user->requires==RTCM104 && (channel->packet_type==RTCM_PACKET))
	return true;
    else if (user->requires == GPS 
	     && (channel->packet_type!=RTCM_PACKET) && (channel->packet_type!=BAD_PACKET))
	return true;
    else
	return false;
    return true;
}

/*@ -branchstate -usedef -globstate @*/
static bool assign_channel(struct subscriber_t *user)
{
    /* if subscriber has no device... */
    if (user->device == NULL) {
	double most_recent = 0;
	struct gps_device_t *channel;

	gpsd_report(4, "client(%d): assigning channel...\n", user-subscribers);
	/* ...connect him to the most recently active device */
	for(channel = channels; channel<channels+MAXDEVICES; channel++)
	    if (allocated_channel(channel)) {
		if (allocation_policy(channel, user, most_recent)) {
		    user->device = channel;
		    most_recent = channel->gpsdata.sentence_time;
		}
	    }
    }

    if (user->device == NULL) {
	gpsd_report(1, "client(%d): channel assignment failed.\n", user-subscribers);
	return false;
    }

    /* and open that device */
    if (user->device->gpsdata.gps_fd != -1) 
	gpsd_report(1,"client(%d): channel %d already active.\n",
		    user-subscribers, user->device->gpsdata.gps_fd);
    else {
	gpsd_deactivate(user->device);
	if (gpsd_activate(user->device) < 0) {
	    
	    gpsd_report(1, "client(%d): channel activation failed.\n", user-subscribers);
	    return false;
	} else {
	    gpsd_report(4, "flagging descriptor %d in assign_channel\n", user->device->gpsdata.gps_fd);
	    FD_SET(user->device->gpsdata.gps_fd, &all_fds);
	    if (user->watcher && !user->tied) {
		(void)write(user-subscribers, "F=", 2);
		(void)write(user-subscribers, 
			    user->device->gpsdata.gps_device,
			    strlen(user->device->gpsdata.gps_device));
		(void)write(user-subscribers, "\r\n", 2);
	    }
	    notify_watchers(user->device, "GPSD,X=%f\r\n", timestamp());
	}
    }

    return true;
}
/*@ +branchstate +usedef +globstate @*/

#ifdef RTCM104_SERVICE
static int handle_dgpsip_request(int cfd UNUSED, char *buf UNUSED, int buflen UNUSED)
/* interpret a client request; cfd is the socket back to the client */
{
    return 0;	/* not actually interpreting these yet */
}
#endif /* RTCM104_SERVICE */

static /*@ observer @*/ char *snarfline(char *p, /*@out@*/char **out)
/* copy the rest of the command line, before CR-LF */
{
    char *q;
    static char	stash[BUFSIZ];

    /*@ -temptrans -mayaliasunique @*/
    for (q = p; isprint(*p) && !isspace(*p) && /*@i@*/(p-q < BUFSIZ-1); p++)
	continue;
    (void)memcpy(stash, q, (size_t)(p-q));
    stash[p-q] = '\0';
    *out = stash;
    return p;
    /*@ +temptrans +mayaliasunique @*/
}

static bool privileged_user(struct subscriber_t *who)
/* is this user privileged to change the GPS's behavior? */
{
    struct subscriber_t *sub;
    int subscribercount = 0;

    /* grant user privilege if he's the only one on the channel */
    for (sub = subscribers; 
	 	sub < subscribers + sizeof(subscribers)/sizeof(subscribers[0]);
	 	sub++)
	if (sub->device == who->device)
	    subscribercount++;
    return (subscribercount == 1);
}

static int handle_gpsd_request(int cfd, char *buf, int buflen)
/* interpret a client request; cfd is the socket back to the client */
{
    char reply[BUFSIZ], phrase[BUFSIZ], *p, *stash;
    int i, j;
    struct subscriber_t *whoami = subscribers + cfd;
    struct gps_device_t *newchan;

    (void)strcpy(reply, "GPSD");
    p = buf;
    while (*p != '\0' && p - buf < buflen) {
	phrase[0] = '\0';
	switch (toupper(*p++)) {
	case 'A':
	    if (assign_channel(whoami) && 
			have_fix(whoami->device) && 
			whoami->device->gpsdata.fix.mode == MODE_3D)
		(void)snprintf(phrase, sizeof(phrase), ",A=%.3f", 
			whoami->device->gpsdata.fix.altitude);
	    else
		(void)strcpy(phrase, ",A=?");
	    break;
	case 'B':		/* change baud rate (SiRF/Zodiac only) */
	    if (assign_channel(whoami) && whoami->device->device_type!=NULL && *p=='=' && privileged_user(whoami)) {
		i = atoi(++p);
		while (isdigit(*p)) p++;
		if (whoami->device->device_type->speed_switcher)
		    if (whoami->device->device_type->speed_switcher(whoami->device, (unsigned)i)) {
			/* 
			 * Allow the control string time to register at the
			 * GPS before we do the baud rate switch, which 
			 * effectively trashes the UART's buffer.
			 *
			 * This definitely fails below 40 milliseconds on a
			 * BU-303b. 50ms is also verified by Chris Kuethe on 
			 *        Pharos iGPS360 + GSW 2.3.1ES + prolific
			 *        Rayming TN-200 + GSW 2.3.1 + ftdi
			 *        Rayming TN-200 + GSW 2.3.2 + ftdi
			 * so it looks pretty solid.
			 *
			 * The minimum delay time is probably constant
			 * across any given type of UART.
			 */
			(void)tcdrain(whoami->device->gpsdata.gps_fd);
			(void)usleep(50000);
			gpsd_set_speed(whoami->device, (speed_t)i,
				(unsigned char)whoami->device->gpsdata.parity,
				whoami->device->gpsdata.stopbits);
		    }
	    }
	    if (whoami->device) {
		if ( whoami->device->gpsdata.parity == 0 ) {
			/* zero parity breaks the next snprintf */
			whoami->device->gpsdata.parity = (unsigned)'N';
		}
		(void)snprintf(phrase, sizeof(phrase), ",B=%d %d %c %u", 
		    (int)gpsd_get_speed(&whoami->device->ttyset),
			9 - whoami->device->gpsdata.stopbits, 
			(int)whoami->device->gpsdata.parity,
			whoami->device->gpsdata.stopbits);
	    } else {
		(void)strcpy(phrase, ",B=?");
	    }
	    break;
	case 'C':
	    if (!assign_channel(whoami) || whoami->device->device_type==NULL)
		(void)strcpy(phrase, ",C=?");
	    else {
		struct gps_type_t *dev = whoami->device->device_type;
		double mincycle = (dev->cycle_chars * 10.0) / whoami->device->gpsdata.baudrate;
		if (*p == '=' && privileged_user(whoami)) {
		    double cycle = strtod(++p, &p);
		    if (cycle >= mincycle)
			if (dev->rate_switcher != NULL)
			    if (dev->rate_switcher(whoami->device, cycle))
				dev->cycle = cycle;
		}
		if (dev->rate_switcher == NULL)
		    (void)snprintf(phrase, sizeof(phrase), 
				   ",C=%.2f", dev->cycle);
		else
		    (void)snprintf(phrase, sizeof(phrase), 
				   ",C=%.2f %.2f", dev->cycle, mincycle);
	    }
	    break;
	case 'D':
	    (void)strcpy(phrase, ",D=");
	    if (assign_channel(whoami) && isnan(whoami->device->gpsdata.fix.time)==0)
		(void)unix_to_iso8601(whoami->device->gpsdata.fix.time, 
				phrase+3, (int)(sizeof(phrase)-3));
	    else
		(void)strcat(phrase, "?");
	    break;
	case 'E':
	    (void)strcpy(phrase, ",E=?");
	    if (assign_channel(whoami) && have_fix(whoami->device))
		(void)snprintf(phrase, sizeof(phrase), ",E=%.2f %.2f %.2f", 
			       whoami->device->gpsdata.epe, 
			       whoami->device->gpsdata.fix.eph, 
			       whoami->device->gpsdata.fix.epv);
	    break;
	case 'F':
	    /*@ -branchstate @*/
	    if (*p == '=') {
		p = snarfline(++p, &stash);
		gpsd_report(1,"<= client(%d): switching to %s\n",cfd,stash);
		if ((newchan = find_device(stash))) {
		    /*@i@*/whoami->device = newchan;
		    whoami->tied = true;
		}
	    }
	    /*@ +branchstate @*/
	    if (whoami->device != NULL)
		(void)snprintf(phrase, sizeof(phrase), ",F=%s", 
			 whoami->device->gpsdata.gps_device);
	    else
		(void)strcpy(phrase, ",F=?");
	    break;
	case 'G':
	    if (*p == '=') {
		gpsd_report(1,"<= client(%d): requesting data type %s\n",cfd,++p);
		if (strncasecmp(p, "rtcm104", 7) == 0)
		    whoami->requires = RTCM104;
		else if (strncasecmp(p, "gps", 3) == 0)
		    whoami->requires = GPS;
		else
		    whoami->requires = ANY;
		p += strcspn(p, ",\r\n");
	    }
	    (void)assign_channel(whoami);
	    if (whoami->device==NULL||whoami->device->packet_type==BAD_PACKET)
		(void)strcpy(phrase, ",G=?");
	    else if (whoami->device->packet_type == RTCM_PACKET)
		(void)snprintf(phrase, sizeof(phrase), ",G=RTCM104");
	    else
		(void)snprintf(phrase, sizeof(phrase), ",G=GPS");
	    break;
	case 'I':
	    if (assign_channel(whoami) && whoami->device->device_type!=NULL)
		(void)snprintf(phrase, sizeof(phrase), ",I=%s", 
			 whoami->device->device_type->typename);
	    else
		(void)strcpy(phrase, ",I=?");
	    break;
	case 'K':
	    for (j = i = 0; i < MAXDEVICES; i++)
		if (allocated_channel(&channels[i]))
		    j++;
	    (void)snprintf(phrase, sizeof(phrase), ",K=%d ", j);
	    for (i = 0; i < MAXDEVICES; i++) {
		if (allocated_channel(&channels[i]) && strlen(phrase)+strlen(channels[i].gpsdata.gps_device)+1 < sizeof(phrase)) {
		    (void)strcat(phrase, channels[i].gpsdata.gps_device);
		    (void)strcat(phrase, " ");
		}
	    }
	    phrase[strlen(phrase)-1] = '\0';
	    break;
	case 'L':
	    (void)snprintf(phrase, sizeof(phrase), ",L=2 " VERSION " abcdefgiklmnopqrstuvwxyz");	//hj
	    break;
	case 'M':
	    if (!assign_channel(whoami) && (!whoami->device || whoami->device->gpsdata.fix.mode == MODE_NOT_SEEN))
		(void)strcpy(phrase, ",M=?");
	    else
		(void)snprintf(phrase, sizeof(phrase), ",M=%d", whoami->device->gpsdata.fix.mode);
	    break;
	case 'N':
	    if (!assign_channel(whoami) || whoami->device->device_type == NULL)
		(void)strcpy(phrase, ",N=?");
	    else if (!whoami->device->device_type->mode_switcher)
		(void)strcpy(phrase, ",N=0");
	    else if (privileged_user(whoami)) {
		if (*p == '=') ++p;
		if (*p == '1' || *p == '+') {
		    whoami->device->device_type->mode_switcher(whoami->device, 1);
		    p++;
		} else if (*p == '0' || *p == '-') {
		    whoami->device->device_type->mode_switcher(whoami->device, 0);
		    p++;
		}
	    }
	    if (!whoami->device)
		(void)snprintf(phrase, sizeof(phrase), ",N=?");
	    else
		(void)snprintf(phrase, sizeof(phrase), ",N=%u", whoami->device->gpsdata.driver_mode);
	    break;
	case 'O':
	    if (!assign_channel(whoami) || !have_fix(whoami->device))
		(void)strcpy(phrase, ",O=?");
	    else {
		(void)snprintf(phrase, sizeof(phrase), ",O=%s",
			       whoami->device->gpsdata.tag[0]!='\0' ? whoami->device->gpsdata.tag : "-");
		if (isnan(whoami->device->gpsdata.fix.time)==0)
		    (void)snprintf(phrase+strlen(phrase),
				   sizeof(phrase)-strlen(phrase),
				   " %.2f",
				   whoami->device->gpsdata.fix.time);
		else
		    (void)strcat(phrase, "          ?");
		if (isnan(whoami->device->gpsdata.fix.ept)==0)
		    (void)snprintf(phrase+strlen(phrase),
				   sizeof(phrase)-strlen(phrase),
				   " %.3f",
				   whoami->device->gpsdata.fix.ept);
		else
		    (void)strcat(phrase, "          ?");
		if (isnan(whoami->device->gpsdata.fix.latitude)==0)
		    (void)snprintf(phrase+strlen(phrase),
				   sizeof(phrase)-strlen(phrase),
				   " %.6f",
				   whoami->device->gpsdata.fix.latitude);
		else
		    (void)strcat(phrase, "          ?");
		if (isnan(whoami->device->gpsdata.fix.longitude)==0)
		    (void)snprintf(phrase+strlen(phrase),
				   sizeof(phrase)-strlen(phrase),
				   " %.6f",
				   whoami->device->gpsdata.fix.longitude);
		else
		    (void)strcat(phrase, "          ?");
		if (isnan(whoami->device->gpsdata.fix.altitude)==0)
		    (void)snprintf(phrase+strlen(phrase),
				   sizeof(phrase)-strlen(phrase),
				   " %7.2f",
				   whoami->device->gpsdata.fix.altitude);
		else
		    (void)strcat(phrase, "          ?");
		if (isnan(whoami->device->gpsdata.fix.eph)==0)
		    (void)snprintf(phrase+strlen(phrase), 
				   sizeof(phrase)-strlen(phrase),
				  " %5.2f",  whoami->device->gpsdata.fix.eph);
		else
		    (void)strcat(phrase, "        ?");
		if (isnan(whoami->device->gpsdata.fix.epv)==0)
		    (void)snprintf(phrase+strlen(phrase), 
				   sizeof(phrase)-strlen(phrase),
				   " %5.2f",  whoami->device->gpsdata.fix.epv);
		else
		    (void)strcat(phrase, "        ?");
		if (isnan(whoami->device->gpsdata.fix.track)==0)
		    (void)snprintf(phrase+strlen(phrase), 
				   sizeof(phrase)-strlen(phrase),
				   " %8.4f %8.3f",
				   whoami->device->gpsdata.fix.track, 
				   whoami->device->gpsdata.fix.speed);
		else
		    (void)strcat(phrase, "             ?            ?");
		if (isnan(whoami->device->gpsdata.fix.climb)==0)
		    (void)snprintf(phrase+strlen(phrase),
				   sizeof(phrase)-strlen(phrase),
				   " %6.3f", 
				   whoami->device->gpsdata.fix.climb);
		else
		    (void)strcat(phrase, "          ?");
		if (isnan(whoami->device->gpsdata.fix.epd)==0)
		    (void)snprintf(phrase+strlen(phrase), 
				   sizeof(phrase)-strlen(phrase),
				   " %8.4f",
				   whoami->device->gpsdata.fix.epd);
		else
		    (void)strcat(phrase, "             ?");
		if (isnan(whoami->device->gpsdata.fix.eps)==0)
		    (void)snprintf(phrase+strlen(phrase),
			     sizeof(phrase)-strlen(phrase),
			     " %5.2f", whoami->device->gpsdata.fix.eps);		    
		else
		    (void)strcat(phrase, "        ?");
		if (isnan(whoami->device->gpsdata.fix.epc)==0)
		    (void)snprintf(phrase+strlen(phrase),
			     sizeof(phrase)-strlen(phrase),
			     " %5.2f", whoami->device->gpsdata.fix.epc);		    
		else
		    (void)strcat(phrase, "        ?");
	    }
	    break;
	case 'P':
	    if (assign_channel(whoami) && have_fix(whoami->device))
		(void)snprintf(phrase, sizeof(phrase), ",P=%.6f %.6f", 
			whoami->device->gpsdata.fix.latitude, 
			whoami->device->gpsdata.fix.longitude);
	    else
		(void)strcpy(phrase, ",P=?");
	    break;
	case 'Q':
#define ZEROIZE(x)	(isnan(x)!=0 ? 0.0 : x)  
	    if (assign_channel(whoami) && 
		(isnan(whoami->device->gpsdata.pdop)==0
		 || isnan(whoami->device->gpsdata.hdop)==0
		 || isnan(whoami->device->gpsdata.vdop)==0))
		(void)snprintf(phrase, sizeof(phrase), ",Q=%d %.2f %.2f %.2f %.2f %.2f",
			whoami->device->gpsdata.satellites_used, 
			ZEROIZE(whoami->device->gpsdata.pdop), 
			ZEROIZE(whoami->device->gpsdata.hdop), 
			ZEROIZE(whoami->device->gpsdata.vdop),
			ZEROIZE(whoami->device->gpsdata.tdop),
			ZEROIZE(whoami->device->gpsdata.gdop));
	    else
		(void)strcpy(phrase, ",Q=?");
#undef ZEROIZE
	    break;
	case 'R':
	    if (*p == '=') ++p;
	    if (*p == '2') {
		(void)assign_channel(whoami);
		subscribers[cfd].raw = 2;
		gpsd_report(3, "client(%d) turned on super-raw mode\n", cfd);
		(void)snprintf(phrase, sizeof(phrase), ",R=2");
		p++;
	    } else if (*p == '1' || *p == '+') {
		(void)assign_channel(whoami);
		subscribers[cfd].raw = 1;
		gpsd_report(3, "client(%d) turned on raw mode\n", cfd);
		(void)snprintf(phrase, sizeof(phrase), ",R=1");
		p++;
	    } else if (*p == '0' || *p == '-') {
		subscribers[cfd].raw = 0;
		gpsd_report(3, "client(%d) turned off raw mode\n", cfd);
		(void)snprintf(phrase, sizeof(phrase), ",R=0");
		p++;
	    } else if (subscribers[cfd].raw) {
		subscribers[cfd].raw = 0;
		gpsd_report(3, "client(%d) turned off raw mode\n", cfd);
		(void)snprintf(phrase, sizeof(phrase), ",R=0");
	    } else {
		(void)assign_channel(whoami);
		subscribers[cfd].raw = 1;
		gpsd_report(3, "client(%d) turned on raw mode\n", cfd);
		(void)snprintf(phrase, sizeof(phrase), ",R=1");
	    }
	    break;
	case 'S':
	    if (assign_channel(whoami))
		(void)snprintf(phrase, sizeof(phrase), ",S=%d", whoami->device->gpsdata.status);
	    else
		(void)strcpy(phrase, ",S=?");
	    break;
	case 'T':
	    if (assign_channel(whoami) && have_fix(whoami->device) && isnan(whoami->device->gpsdata.fix.track)==0)
		(void)snprintf(phrase, sizeof(phrase), ",T=%.4f", whoami->device->gpsdata.fix.track);
	    else
		(void)strcpy(phrase, ",T=?");
	    break;
	case 'U':
	    if (assign_channel(whoami) && have_fix(whoami->device) && whoami->device->gpsdata.fix.mode == MODE_3D)
		(void)snprintf(phrase, sizeof(phrase), ",U=%.3f", whoami->device->gpsdata.fix.climb);
	    else
		(void)strcpy(phrase, ",U=?");
	    break;
	case 'V':
	    if (assign_channel(whoami) && have_fix(whoami->device) && isnan(whoami->device->gpsdata.fix.track)==0)
		(void)snprintf(phrase, sizeof(phrase), ",V=%.3f", whoami->device->gpsdata.fix.speed / KNOTS_TO_KPH);
	    else
		(void)strcpy(phrase, ",V=?");
	    break;
	case 'W':
	    if (*p == '=') ++p;
	    if (*p == '1' || *p == '+') {
		subscribers[cfd].watcher = true;
		(void)assign_channel(whoami);
		(void)snprintf(phrase, sizeof(phrase), ",W=1");
		p++;
	    } else if (*p == '0' || *p == '-') {
		subscribers[cfd].watcher = false;
		(void)snprintf(phrase, sizeof(phrase), ",W=0");
		p++;
	    } else if (subscribers[cfd].watcher!=0) {
		subscribers[cfd].watcher = false;
		(void)snprintf(phrase, sizeof(phrase), ",W=0");
	    } else {
		subscribers[cfd].watcher = true;
		(void)assign_channel(whoami);
		gpsd_report(3, "client(%d) turned on watching\n", cfd);
		(void)snprintf(phrase, sizeof(phrase), ",W=1");
	    }
	    break;
        case 'X':
	    if (assign_channel(whoami) && whoami->device != NULL)
		(void)snprintf(phrase, sizeof(phrase), ",X=%f", whoami->device->gpsdata.online);
	    else
		(void)strcpy(phrase, ",X=?");
	    break;
	case 'Y':
	    if (assign_channel(whoami) && whoami->device->gpsdata.satellites > 0) {
		int used, reported = 0;
		(void)strcpy(phrase, ",Y=");
		if (whoami->device->gpsdata.tag[0] != '\0')
		    (void)strcat(phrase, whoami->device->gpsdata.tag);
		else
		    (void)strcat(phrase, "-");
		if (isnan(whoami->device->gpsdata.sentence_time)==0)
		    (void)snprintf(phrase+strlen(phrase), 
				   sizeof(phrase)-strlen(phrase),
				   " %f ",
				   whoami->device->gpsdata.sentence_time);
		else
		    (void)strcat(phrase, " ? ");
		(void)snprintf(phrase+strlen(phrase), 
			       sizeof(phrase)-strlen(phrase),
			       "%d:", whoami->device->gpsdata.satellites);
		for (i = 0; i < whoami->device->gpsdata.satellites; i++) {
		    used = 0;
		    for (j = 0; j < whoami->device->gpsdata.satellites_used; j++)
			if (whoami->device->gpsdata.used[j] == whoami->device->gpsdata.PRN[i]) {
			    used = 1;
			    break;
			}
		    if (whoami->device->gpsdata.PRN[i]) {
			(void)snprintf(phrase+strlen(phrase), 
				      sizeof(phrase)-strlen(phrase),
				      "%d %d %d %d %d:", 
				      whoami->device->gpsdata.PRN[i], 
				      whoami->device->gpsdata.elevation[i],whoami->device->gpsdata.azimuth[i],
				      whoami->device->gpsdata.ss[i],
				      used);
			reported++;
		    }
		}
		if (whoami->device->gpsdata.satellites != reported)
		    gpsd_report(1,"Satellite count %d != PRN count %d\n",
				whoami->device->gpsdata.satellites, reported);
	    } else
		(void)strcpy(phrase, ",Y=?");
	    break;
	case 'Z':
	    (void)assign_channel(whoami); 
	    if (*p == '=') ++p;
	    if (whoami->device == NULL) {
		(void)snprintf(phrase, sizeof(phrase), ",Z=?");
		p++;		
	    } else if (*p == '1' || *p == '+') {
		whoami->device->gpsdata.profiling = true;
		gpsd_report(3, "client(%d) turned on profiling mode\n", cfd);
		(void)snprintf(phrase, sizeof(phrase), ",Z=1");
		p++;
	    } else if (*p == '0' || *p == '-') {
		whoami->device->gpsdata.profiling = false;
		gpsd_report(3, "client(%d) turned off profiling mode\n", cfd);
		(void)snprintf(phrase, sizeof(phrase), ",Z=0");
		p++;
	    } else {
		whoami->device->gpsdata.profiling = !whoami->device->gpsdata.profiling;
		gpsd_report(3, "client(%d) toggled profiling mode\n", cfd);
		(void)snprintf(phrase, sizeof(phrase), ",Z=%d",
			       (int)whoami->device->gpsdata.profiling);
	    }
	    break;
        case '$':
	    if (whoami->device->gpsdata.sentence_time!=0)
		(void)snprintf(phrase, sizeof(phrase), ",$=%s %d %f %f %f %f %f %f",
			whoami->device->gpsdata.tag,
			(int)whoami->device->gpsdata.sentence_length,
			whoami->device->gpsdata.sentence_time,
			whoami->device->gpsdata.d_xmit_time - whoami->device->gpsdata.sentence_time,
			whoami->device->gpsdata.d_recv_time - whoami->device->gpsdata.sentence_time,
			whoami->device->gpsdata.d_decode_time - whoami->device->gpsdata.sentence_time,
			whoami->device->poll_times[cfd] - whoami->device->gpsdata.sentence_time,
			timestamp() - whoami->device->gpsdata.sentence_time);
	    else
		(void)snprintf(phrase, sizeof(phrase), ",$=%s %d 0 %f %f %f %f %f",
			whoami->device->gpsdata.tag,
			(int)whoami->device->gpsdata.sentence_length,
			whoami->device->gpsdata.d_xmit_time,
			whoami->device->gpsdata.d_recv_time - whoami->device->gpsdata.d_xmit_time,
			whoami->device->gpsdata.d_decode_time - whoami->device->gpsdata.d_xmit_time,
			whoami->device->poll_times[cfd] - whoami->device->gpsdata.d_xmit_time,
			timestamp() - whoami->device->gpsdata.d_xmit_time);
	    break;
	case '\r': case '\n':
	    goto breakout;
	}
	if (strlen(reply) + strlen(phrase) < sizeof(reply) - 1)
	    (void)strcat(reply, phrase);
	else
	    return -1;	/* Buffer would overflow.  Just return an error */
    }
 breakout:
    (void)strcat(reply, "\r\n");

    return (int)throttled_write(cfd, reply, (ssize_t)strlen(reply));
}

static void handle_control(int sfd, char *buf)
/* handle privileged commands coming through the control socket */
{
    char	*p, *stash, *eq;
    struct gps_device_t	*chp;
    int cfd;

    if (buf[0] == '-') {
	p = snarfline(buf+1, &stash);
	gpsd_report(1,"<= control(%d): removing %s\n", sfd, stash);
	if ((chp = find_device(stash))) {
	    if (chp->gpsdata.gps_fd > 0)
		FD_CLR(chp->gpsdata.gps_fd, &all_fds);
	    notify_watchers(chp, "X=0\r\n");
	    for (cfd = 0; cfd < FD_SETSIZE; cfd++)
		if (subscribers[cfd].device == chp)
		    subscribers[cfd].device = NULL;
	    gpsd_wrap(chp);
	    /*@i@*/free_channel(chp);	/* modifying observer storage */
	    (void)write(sfd, "OK\n", 3);
	} else
	    (void)write(sfd, "ERROR\n", 6);
    } else if (buf[0] == '+') {
	p = snarfline(buf+1, &stash);
	if (find_device(stash))
	    gpsd_report(1,"<= control(%d): %s already active \n", sfd, stash);
	else {
	    gpsd_report(1,"<= control(%d): adding %s \n", sfd, stash);
	    if (open_device(stash))
		(void)write(sfd, "OK\n", 3);
	    else
		(void)write(sfd, "ERROR\n", 6);
	}
    } else if (buf[0] == '!') {
	p = snarfline(buf+1, &stash);
	eq = strchr(stash, '=');
	if (!eq) {
	    gpsd_report(1,"<= control(%d): ill-formed command \n", sfd);
	    (void)write(sfd, "ERROR\n", 3);
	} else {
	    *eq++ = '\0';
	    if ((chp = find_device(stash))) {
		gpsd_report(1,"<= control(%d): writing to %s \n", sfd, stash);
		(void)write(chp->gpsdata.gps_fd, eq, strlen(eq));
		(void)write(sfd, "OK\n", 3);
	    } else {
		gpsd_report(1,"<= control(%d): %s not active \n", sfd, stash);
		(void)write(sfd, "ERROR\n", 6);
	    }
	}
    }
}

/*@ -mustfreefresh @*/
int main(int argc, char *argv[])
{
    static char *pid_file = NULL;
    static bool nowait = false;
    static int st, csock = -1;
    static gps_mask_t changed;
    static char *dgpsserver = NULL;
    static char *gpsd_service = NULL; 
#ifdef RTCM104_SERVICE
    static char *rtcm_service = NULL; 
    static int nsock, rsock = -1;
#endif /* RTCM104_SERVICE */
    static char *control_socket = NULL;
    struct gps_device_t *device, *channel;
    struct sockaddr_in fsin;
    fd_set rfds, control_fds;
    int i, option, msock, cfd, dfd; 
    bool go_background = true;
    struct timeval tv;
    // extern char *optarg;
#ifdef RTCM104_ENABLE
    struct gps_device_t *gps;
#endif /* RTCM104_ENABLE */

    debuglevel = 0;
    while ((option = getopt(argc, argv, "F:D:S:d:fhNnpP:v"
#ifdef RTCM104_SERVICE
			    "R:"
#endif /* RTCM104_SERVICE */
		)) != -1) {
	switch (option) {
	case 'D':
	    debuglevel = (int) strtol(optarg, 0, 0);
	    break;
	case 'F':
	    control_socket = optarg;
	    break;
	case 'N':
	    go_background = false;
	    break;
#ifdef RTCM104_SERVICE
	case 'R':
	    rtcm_service = optarg;
	    break;
#endif /* RTCM104_SERVICE */
	case 'S':
	    gpsd_service = optarg;
	    break;
	case 'd':
	    dgpsserver = optarg;
	    break;
	case 'n':
	    nowait = true;
	    break;
	case 'f':
	case 'p':
	    /* skip this option, treat following as argument */ 
	    break;
	case 'P':
	    pid_file = optarg;
	    break;
	case 'v':
	    (void)printf("gpsd %s\n", VERSION);
	    exit(0);
	case 'h': case '?':
	default:
	    usage();
	    exit(0);
	}
    }

    if (!control_socket && optind >= argc) {
	gpsd_report(0, "can't run with neither control socket nor devices\n");
	exit(1);
    }

    /*
     * Control socket has to be created before we go background in order to
     * avoid a race condition in which hotplug scripts can try opening
     * the socket before it's created.
     */
    if (control_socket) {
	(void)unlink(control_socket);
	if ((csock = filesock(control_socket)) < 0) {
	    gpsd_report(0,"control socket create failed, netlib error %d\n",csock);
	    exit(2);
	}
	FD_SET(csock, &all_fds);
	gpsd_report(1, "control socket opened at %s\n", control_socket);
    }

    if (go_background)
	(void)daemonize();

    if (pid_file) {
	FILE *fp;

	if ((fp = fopen(pid_file, "w")) != NULL) {
	    (void)fprintf(fp, "%u\n", (unsigned int)getpid());
	    (void)fclose(fp);
	} else {
	    gpsd_report(1, "Cannot create PID file: %s.\n", pid_file);
	}
    }

    openlog("gpsd", LOG_PID, LOG_USER);
    gpsd_report(1, "launching (Version %s)\n", VERSION);
    /*@ -observertrans @*/
    if (!gpsd_service)
	gpsd_service = getservbyname("gpsd", "tcp") ? "gpsd" : DEFAULT_GPSD_PORT;
    /*@ +observertrans @*/
    if ((msock = passivesock(gpsd_service, "tcp", QLEN)) < 0) {
	gpsd_report(0,"command socket create failed, netlib error %d\n",msock);
	exit(2);
    }
    gpsd_report(1, "listening on port %s\n", gpsd_service);
#ifdef RTCM104_SERVICE
    /*@ -observertrans @*/
    if (!rtcm_service)
	rtcm_service = getservbyname("rtcm", "tcp") ? "rtcm" : DEFAULT_RTCM_PORT;
    /*@ +observertrans @*/
    if ((nsock = passivesock(rtcm_service, "tcp", QLEN)) < 0) {
	gpsd_report(0,"RTCM104 socket create failed, netlib error %d\n",nsock);
	exit(2);
    }
    gpsd_report(1, "listening on port %s\n", rtcm_service);
#endif /* RTCM104_SERVICE */

    if (dgpsserver) {
        int dsock = dgpsip_open(&context, dgpsserver);
	if (dsock >= 0)
	    FD_SET(dsock, &all_fds);
    }

#ifdef NTPSHM_ENABLE
    if (getuid() == 0) {
	(void)nice(-10);		/* for precise timekeeping increase priority */
	(void)ntpshm_init(&context, nowait);
    }
#endif /* NTPSHM_ENABLE */

#if DBUS_ENABLE
    /* we need to connect to dbus as root */
    if (initialize_dbus_connection()) {
	/* the connection could not be started */
	gpsd_report (2, "unable to connect to the DBUS system bus\n");
    } else
	gpsd_report (2, "successfully connected to the DBUS system bus\n");
#endif /* DBUS_ENABLE */

    if (getuid() == 0 && go_background) {
	struct passwd *pw;
	struct stat stb;

	/* make default devices accessible even after we drop privileges */
	for (i = optind; i < argc; i++) 
	    if (stat(argv[i], &stb) == 0)
		(void)chmod(argv[i], stb.st_mode|S_IRGRP|S_IWGRP);
	/*
	 * Drop privileges.  Up to now we've been running as root.  Instead,
	 * set the user ID to 'nobody' and the group ID to the owning group 
	 * of a prototypical TTY device.  This limits the scope of any
	 * compromises in the code.  It requires that all GPS devices have
	 * their group read/write permissions set.
	 */
	if ((optind<argc&&stat(argv[optind], &stb)==0)||stat(PROTO_TTY,&stb)==0) {
	    gpsd_report(2, "changing to group %d\n", stb.st_gid);
	    if (setgid(stb.st_gid) != 0)
		gpsd_report(0, "setgid() failed, errno %s\n", strerror(errno));
	}
	pw = getpwnam("nobody");
	if (pw)
	    (void)setuid(pw->pw_uid);
    }
    gpsd_report(2, "running with effective group ID %d\n", getegid());
    gpsd_report(2, "running with effective user ID %d\n", geteuid());

    /* user may want to re-initialize all channels */
    if ((st = setjmp(restartbuf)) > 0) {
	for (dfd = 0; dfd < FD_SETSIZE; dfd++) {
	    if (allocated_channel(&channels[dfd]))
		(void)gpsd_wrap(&channels[dfd]);
	}
	if (st == SIGHUP+1)
	    gpsd_report(1, "gpsd restarted by SIGHUP\n");
	else if (st > 0) {
	    gpsd_report(1,"Received terminating signal %d. Exiting...\n",st-1);
	    if (control_socket)
		(void)unlink(control_socket);
	    if (pid_file)
		(void)unlink(pid_file);
	    exit(10 + st);
	}
    }

    /* Handle some signals */
    (void)signal(SIGHUP, onsig);
    (void)signal(SIGINT, onsig);
    (void)signal(SIGTERM, onsig);
    (void)signal(SIGQUIT, onsig);
    (void)signal(SIGPIPE, SIG_IGN);

    FD_SET(msock, &all_fds);
#ifdef RTCM104_SERVICE
    FD_SET(nsock, &all_fds);
#endif /* RTCM104_SERVICE */
    FD_ZERO(&control_fds);

    /* optimization hack to defer having to read subframe data */
    if (time(NULL) < START_SUBFRAME)
	context.valid |= LEAP_SECOND_VALID;

    for (i = optind; i < argc; i++) { 
	device = open_device(argv[i]);
	if (!device) {
	    gpsd_report(0, "GPS device %s nonexistent or can't be read\n", argv[i]);
	}
    }

    for (;;) {
        (void)memcpy((char *)&rfds, (char *)&all_fds, sizeof(rfds));

	gpsd_report(7, "select waits\n");
	/* 
	 * Poll for user commands or GPS data.  The timeout doesn't
	 * actually matter here since select returns whenever one of
	 * the file descriptors in the set goes ready. 
	 */
	/*@ -usedef @*/
	tv.tv_sec = 1; tv.tv_usec = 0;
	if (select(FD_SETSIZE, &rfds, NULL, NULL, &tv) < 0) {
	    if (errno == EINTR)
		continue;
	    gpsd_report(0, "select: %s\n", strerror(errno));
	    exit(2);
	}
	/*@ +usedef @*/

#ifdef __UNUSED__
	{
	    char dbuf[BUFSIZ];
	    dbuf[0] = '\0';
	    for (cfd = 0; cfd < FD_SETSIZE; cfd++)
		if (FD_ISSET(cfd, &all_fds))
		    (void)snprintf(dbuf + strlen(dbuf), 
				   sizeof(dbuf)-strlen(dbuf),
				   " %d", cfd);
	    strcat(dbuf, "} -> {");
	    for (cfd = 0; cfd < FD_SETSIZE; cfd++)
		if (FD_ISSET(cfd, &rfds))
		    (void)snprintf(dbuf + strlen(dbuf), 
				   sizeof(dbuf)-strlen(dbuf),
				   " %d", cfd);
	    gpsd_report(4, "Polling descriptor set: {%s}\n", dbuf);
	}
#endif /* UNUSED */

	/* always be open to new client connections */
	if (FD_ISSET(msock, &rfds)) {
	    socklen_t alen = (socklen_t)sizeof(fsin);
	    /*@i1@*/int ssock = accept(msock, (struct sockaddr *) &fsin, &alen);

	    if (ssock < 0)
		gpsd_report(0, "accept: %s\n", strerror(errno));
	    else {
		int opts = fcntl(ssock, F_GETFL);

		if (opts >= 0)
		    (void)fcntl(ssock, F_SETFL, opts | O_NONBLOCK);
		gpsd_report(3, "client connect on %d\n", ssock);
		FD_SET(ssock, &all_fds);
		subscribers[ssock].active = timestamp();
		subscribers[ssock].tied = false;
		subscribers[ssock].requires = ANY;
	    }
	    FD_CLR(msock, &rfds);
	}

#ifdef RTCM104_SERVICE
	/* also to RTCM client connections */
	if (FD_ISSET(nsock, &rfds)) {
	    socklen_t alen = (socklen_t)sizeof(fsin);
	    /*@i1@*/int ssock = accept(nsock, (struct sockaddr *)&fsin, &alen);

	    if (rsock < 0)
		gpsd_report(0, "accept: %s\n", strerror(errno));
	    else {
		int opts = fcntl(rsock, F_GETFL);

		if (opts >= 0)
		    (void)fcntl(rsock, F_SETFL, opts | O_NONBLOCK);
		gpsd_report(3, "client connect on %d\n", rsock);
		FD_SET(ssock, &all_fds);
		subscribers[rsock].active = true;
		subscribers[rsock].tied = false;
		subscribers[rsock].requires = RTCM104;
	    }
	    FD_CLR(nsock, &rfds);
	}
#endif /* RTCM104_SERVICE */

	/* also be open to new control-socket connections */
	if (csock > -1 && FD_ISSET(csock, &rfds)) {
	    socklen_t alen = (socklen_t)sizeof(fsin);
	    /*@i1@*/int ssock = accept(csock, (struct sockaddr *) &fsin, &alen);

	    if (ssock < 0)
		gpsd_report(0, "accept: %s\n", strerror(errno));
	    else {
		gpsd_report(3, "control socket connect on %d\n", ssock);
		FD_SET(ssock, &all_fds);
		FD_SET(ssock, &control_fds);
	    }
	    FD_CLR(csock, &rfds);
	}

	/* be ready for DGPSIP reports */
	if (context.dsock >= 0 && FD_ISSET(context.dsock, &rfds))
	    dgpsip_poll(&context);

	/* read any commands that came in over control sockets */
	for (cfd = 0; cfd < FD_SETSIZE; cfd++)
	    if (FD_ISSET(cfd, &control_fds)) {
		char buf[BUFSIZ];

		while (read(cfd, buf, sizeof(buf)-1) > 0) {
		    gpsd_report(1, "<= control(%d): %s\n", cfd, buf);
		    handle_control(cfd, buf);
		}
		(void)close(cfd);
		FD_CLR(cfd, &all_fds);
		FD_CLR(cfd, &control_fds);
	    }

	/* poll all active devices */
	for (channel = channels; channel < channels + MAXDEVICES; channel++) {
	    if (!allocated_channel(channel))
		continue;

	    /* pass the current DGPSIP correction to the GPS if new */
	    if (channel->device_type)
		dgpsip_relay(channel);

	    /* get data from the device */
	    changed = 0;
	    if (channel->gpsdata.gps_fd >= 0 && FD_ISSET(channel->gpsdata.gps_fd, &rfds))
	    {
		gpsd_report(5, "polling %d\n", channel->gpsdata.gps_fd);
		changed = gpsd_poll(channel);
		if (changed == ERROR_SET) {
		    gpsd_report(3, "packet sniffer failed to sync up\n");
		    FD_CLR(channel->gpsdata.gps_fd, &all_fds);
		    gpsd_deactivate(channel);
		} 
		if ((changed & ONLINE_SET) == 0) {
/*
		    gpsd_report(3, "GPS is offline (%lf sec since data)\n", 
				timestamp() - channel->gpsdata.online);
*/
		    FD_CLR(channel->gpsdata.gps_fd, &all_fds);
		    gpsd_deactivate(channel);
		    notify_watchers(channel, "GPSD,X=0\r\n");
		}
#ifdef RTCM104_ENABLE
		/* copy each RTCM-104 correction to all GPSes */
		if ((changed & RTCM_SET) != 0) {
		    for (gps = channels; gps < channels + MAXDEVICES; gps++)
			if (gps->device_type != NULL && gps->device_type->rtcm_writer != NULL)
			    (void)gps->device_type->rtcm_writer(gps, (char *)channel->outbuffer, channel->outbuflen);
		}
#endif /* RTCM104_ENABLE */
	    }

	    for (cfd = 0; cfd < FD_SETSIZE; cfd++) {
		/* some listeners may be in watcher mode */
		if (subscribers[cfd].watcher) {
		    char cmds[4] = ""; 
		    channel->poll_times[cfd] = timestamp();
		    if (changed &~ ONLINE_SET) {
			if (changed & (LATLON_SET | MODE_SET))
			    (void)strcat(cmds, "o");
			if (changed & SATELLITE_SET)
			    (void)strcat(cmds, "y");
			if (channel->gpsdata.profiling!=0)
			    (void)strcat(cmds, "$");
		    }
		    if (cmds[0] != '\0')
			(void)handle_gpsd_request(cfd, cmds, (int)strlen(cmds));
		}
	    }
#if DBUS_ENABLE
	    if (changed &~ ONLINE_SET) {
		    if (changed & (LATLON_SET | MODE_SET)) 
			    send_dbus_fix (channel);
	    }
#endif
	}

#ifdef NOT_FIXED
	/* may be time to hunt up a DGPSIP server */
	if (context.fixcnt > 0 && context.dsock == -1) {
	    for (channel=channels; channel < channels+MAXDEVICES; channel++) {
		if (channel->gpsdata.fix.mode > MODE_NO_FIX) {
		    dgpsip_autoconnect(&context,
				       channel->gpsdata.fix.latitude,
				       channel->gpsdata.fix.longitude,
				       DGPS_SERVER_LIST);
		    break;
		}
	    }
	}
#endif

	/* accept and execute commands for all clients */
	for (cfd = 0; cfd < FD_SETSIZE; cfd++) {
	    if (subscribers[cfd].active == 0) 
		continue;

	    if (FD_ISSET(cfd, &rfds)) {
		char buf[BUFSIZ];
		int buflen;

		gpsd_report(3, "checking client(%d)\n", cfd);
		if ((buflen = (int)read(cfd, buf, sizeof(buf) - 1)) <= 0) {
		    detach_client(cfd);
		} else {
		    buf[buflen] = '\0';
		    gpsd_report(1, "<= client(%d): %s", cfd, buf);

#ifdef RTCM104_SERVICE
		    if (subscribers[cfd].rtcm) {
			if (handle_dgpsip_request(cfd, buf, buflen) < 0)
			    detach_client(cfd);
		    } else
#endif /* RTCM104_SERVICE */
		    {
			if (subscribers[cfd].device)
			    subscribers[cfd].device->poll_times[cfd] = timestamp();
			if (handle_gpsd_request(cfd, buf, buflen) < 0)
			    detach_client(cfd);
		    }
		}
	    } else if (subscribers[cfd].device == NULL && timestamp() - subscribers[cfd].active > ASSIGNMENT_TIMEOUT) {
		gpsd_report(1, "client(%d) timed out before assignment request.\n", cfd);
		detach_client(cfd);
	    } else if (subscribers[cfd].device != NULL && !(subscribers[cfd].watcher || subscribers[cfd].raw>0) && timestamp() - subscribers[cfd].active > POLLER_TIMEOUT) {
		gpsd_report(1, "client(%d) timed out on command wait.\n", cfd);
		detach_client(cfd);
	    }
	}

	/*
	 * Close devices with an identified packet type but no remaining 
	 * subscribers.  The reason the test has this particular form is 
	 * so that, immediately after device open, we'll keep reading 
	 * packets until a type is identified even though there are no
	 * subscribers yet.  We need this to happen so that subscribers 
	 * can later choose a device by packet type.
	 */
	if (!nowait)
	    for (channel=channels; channel < channels+MAXDEVICES; channel++) {
		if (allocated_channel(channel)) {
		    if (channel->packet_type != BAD_PACKET) {
			bool need_gps = false;

			for (cfd = 0; cfd < FD_SETSIZE; cfd++)
			    if (subscribers[cfd].device == channel)
				need_gps = true;

			if (!need_gps && channel->gpsdata.gps_fd > -1) {
			    gpsd_report(4, "unflagging descriptor %d in open_device\n", channel->gpsdata.gps_fd);
			    FD_CLR(channel->gpsdata.gps_fd, &all_fds);
			    gpsd_deactivate(channel);
			}
		    }
		}
	    }
    }

    if (control_socket)
	(void)unlink(control_socket);
    if (pid_file)
	(void)unlink(pid_file);
    return 0;
}
/*@ +mustfreefresh @*/
