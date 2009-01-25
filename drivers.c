/* $Id$ */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>

#include "gpsd_config.h"
#include "gpsd.h"

extern struct gps_type_t zodiac_binary;
extern struct gps_type_t ubx_binary;

ssize_t generic_get(struct gps_device_t *session)
{
    return packet_get(session->gpsdata.gps_fd, &session->packet);
}

#if defined(NMEA_ENABLE) || defined(SIRF_ENABLE) || defined(EVERMORE_ENABLE)  || defined(ITRAX_ENABLE)  || defined(NAVCOM_ENABLE)
ssize_t pass_rtcm(struct gps_device_t *session, char *buf, size_t rtcmbytes)
/* most GPSes take their RTCM corrections straight up */
{
    return gpsd_write(session, buf, rtcmbytes);
}
#endif

#ifdef NMEA_ENABLE
/**************************************************************************
 *
 * Generic driver -- straight NMEA 0183
 *
 **************************************************************************/

gps_mask_t nmea_parse_input(struct gps_device_t *session)
{
    if (session->packet.type == COMMENT_PACKET) {
	return 0;
    } else if (session->packet.type == SIRF_PACKET) {
	gpsd_report(LOG_WARN, "SiRF packet seen when NMEA expected.\n");
#ifdef SIRF_ENABLE
	(void)gpsd_switch_driver(session, "SiRF binary");
	return sirf_parse(session, session->packet.outbuffer, session->packet.outbuflen);
#else
	return 0;
#endif /* SIRF_ENABLE */
    } else if (session->packet.type == EVERMORE_PACKET) {
	gpsd_report(LOG_WARN, "EverMore packet seen when NMEA expected.\n");
#ifdef EVERMORE_ENABLE
	(void)gpsd_switch_driver(session, "EverMore binary");
	return evermore_parse(session, session->packet.outbuffer, session->packet.outbuflen);
#else
	return 0;
#endif /* EVERMORE_ENABLE */
    } else if (session->packet.type == NAVCOM_PACKET) {
  gpsd_report(LOG_WARN, "Navcom packet seen when NMEA expected.\n");
#ifdef NAVCOM_ENABLE
	(void)gpsd_switch_driver(session, "Navcom binary");
	return navcom_parse(session, session->packet.outbuffer, session->packet.outbuflen);
#else
	return 0;
#endif /* NAVCOM_ENABLE */
} else if (session->packet.type == GARMIN_PACKET) {
	gpsd_report(LOG_WARN, "Garmin packet seen when NMEA expected.\n");
#ifdef GARMIN_ENABLE
	/* we might never see a trigger, have this as a backstop */
	(void)gpsd_switch_driver(session, "Garmin Serial binary");
	return garmin_ser_parse(session);
#else
	return 0;
#endif /* GARMIN_ENABLE */
    } else if (session->packet.type == NMEA_PACKET) {
	gps_mask_t st = 0;
#ifdef GARMINTXT_ENABLE
	if (session->packet.outbuflen >= 56) {
		if ((char) *session->packet.outbuffer == '@') {
		/* Garmin Simple Text packet received; it starts with '@' is terminated with \r\n and has length 57 bytes */
			(void)gpsd_switch_driver(session, "Garmin Simple Text");
			return garmintxt_parse(session);
		}
	}
#endif /* GARMINTXT_ENABLE */
	gpsd_report(LOG_IO, "<= GPS: %s", session->packet.outbuffer);
	if ((st=nmea_parse((char *)session->packet.outbuffer, session))==0) {
#ifdef NON_NMEA_ENABLE
	    struct gps_type_t **dp;

	    /* maybe this is a trigger string for a driver we know about? */
	    for (dp = gpsd_drivers; *dp; dp++) {
		char	*trigger = (*dp)->trigger;

		if (trigger!=NULL && strncmp((char *)session->packet.outbuffer, trigger, strlen(trigger))==0 && isatty(session->gpsdata.gps_fd)!=0) {
		    gpsd_report(LOG_PROG, "found %s.\n", trigger);
		    (void)gpsd_switch_driver(session, (*dp)->type_name);
		    return 1;
		}
	    }
#endif /* NON_NMEA_ENABLE */
	    gpsd_report(LOG_WARN, "unknown sentence: \"%s\"\n", session->packet.outbuffer);
	}
#ifdef NMEADISC
	if (session->ldisc == 0) {
	    uid_t old;
	    int ldisc = NMEADISC;

#ifdef TIOCSTSTAMP
	    struct tstamps tstamps;
#ifdef PPS_ON_CTS
	    tstamps.ts_set |= TIOCM_CTS;
#else /*!PPS_ON_CTS */
	    tstamps.ts_set |= TIOCM_CAR;
#endif /* PPS_ON_CTS */
	    tstamps.ts_clr = 0;

	    old = geteuid();
	    if (seteuid(0) == -1)
		gpsd_report(LOG_WARN, "can't seteuid(0) - %s", strerror(errno));
	    else
		gpsd_report(LOG_WARN, "seteuid(0) to enable timestamping\n");
	    if (ioctl(session->gpsdata.gps_fd, TIOCSTSTAMP, &tstamps) < 0)
		gpsd_report(LOG_WARN, "can't set kernel timestamping: %s\n",
		    strerror(errno));
	    else
		gpsd_report(LOG_WARN, "activated kernel timestamping\n");
#endif /* TIOCSTSTAMP */
	    if (ioctl(session->gpsdata.gps_fd, TIOCSETD, &ldisc) == -1)
		gpsd_report(LOG_WARN, "can't set nmea discipline: %s\n",
		    strerror(errno));
	    else
		gpsd_report(LOG_WARN, "activated nmea discipline\n");
	/* this is a flag that shows if we've tried the setup */
	session->ldisc = NMEADISC;

	if (old){
	    gpsd_report(LOG_WARN, "giving up euid 0\n");
	    (void)seteuid(old);
	}
	gpsd_report(LOG_WARN, "running with effective user ID %d\n", geteuid());
	}
#endif /*NMEADISC */
#ifdef NTPSHM_ENABLE
	/* this magic number is derived from observation */
	if (session->context->enable_ntpshm &&
	    (st & TIME_SET) != 0 &&
	    (session->gpsdata.fix.time!=session->last_fixtime)) {
	    (void)ntpshm_put(session, session->gpsdata.fix.time);
	    session->last_fixtime = session->gpsdata.fix.time;
	}
#endif /* NTPSHM_ENABLE */
	return st;
    } else
	return 0;
}

static void nmea_probe_subtype(struct gps_device_t *session, unsigned int seq)
{
    /*
     * The reason for splitting these probes up by packet sequence
     * number, interleaving them with the first few packet receives,
     * is because many generic-NMEA devices get confused if you send
     * too much at them in one go.
     *
     * A fast response to an early probe will change drivers so the
     * later ones won't be sent at all.  Thus, for best overall
     * performance, order these to probe for the most popular types
     * soonest.
     *
     * Note: don't make the trigger strings identical to the probe,
     * because some NMEA devices (notably SiRFs) will just echo
     * unknown strings right back at you. A useful dodge is to append
     * a comma to the trigger, because that won't be in the response
     * unless there is actual following data.
     */
    switch (seq) {
#ifdef SIRF_ENABLE
    case 0:
	/*
	 * We used to try to probe for SiRF by issuing "$PSRF105,1"
	 * and expecting "$Ack Input105.".  But it turns out this
	 * only works for SiRF-IIs; SiRF-I and SiRF-III don't respond.
	 * Thus the only reliable probe is to try to flip the SiRF into
	 * binary mode, cluing in the library to revert it on close.
	 */
	(void)nmea_send(session->gpsdata.gps_fd,
			"$PSRF100,0,%d,%d,%d,0",
			session->gpsdata.baudrate,
			9-session->gpsdata.stopbits,
			session->gpsdata.stopbits);
	session->back_to_nmea = true;
	break;
#endif /* SIRF_ENABLE */
#ifdef NMEA_ENABLE
    case 1:
	/* probe for Garmin serial GPS -- expect $PGRMC followed by data*/
	(void)nmea_send(session->gpsdata.gps_fd, "$PGRMCE");
	break;
    case 2:
	/* probe for the FV-18 -- expect $PFEC,GPint followed by data */
	(void)nmea_send(session->gpsdata.gps_fd, "$PFEC,GPint");
	break;
#endif /* NMEA_ENABLE */
#ifdef EVERMORE_ENABLE
    case 3:
	/* Enable checksum and GGA(1s), GLL(0s), GSA(1s), GSV(1s), RMC(1s), VTG(0s), PEMT101(1s) */
	/* EverMore will reply with: \x10\x02\x04\x38\x8E\xC6\x10\x03 */
	(void)gpsd_write(session,
	    "\x10\x02\x12\x8E\x7F\x01\x01\x00\x01\x01\x01\x00\x01\x00\x00\x00\x00\x00\x00\x13\x10\x03", 22);
	break;
#endif /* EVERMORE_ENABLE */
#ifdef ITRAX_ENABLE
    case 4:
	/* probe for iTrax, looking for "$PFST,OK" */
	(void)nmea_send(session->gpsdata.gps_fd, "$PFST");
	break;
#endif /* ITRAX_ENABLE */
#ifdef GPSCLOCK_ENABLE
    case 5:
	/* probe for Furuno Electric GH-79L4-N (GPSClock) */
	(void)nmea_send(session->gpsdata.gps_fd, "$PFEC,GPsrq");
	break;
#endif /* GPSCLOCK_ENABLE */
#ifdef ASHTECH_ENABLE
    case 6:
	/* probe for Ashtech -- expect $PASHR */
	(void)nmea_send(session->gpsdata.gps_fd, "$PASHQ,RID");
	break;
#endif /* ASHTECH_ENABLE */
    default:
	break;
    }
}

static struct gps_type_t nmea = {
    .type_name      = "Generic NMEA",	/* full name of type */
    .trigger	    = NULL,		/* it's the default */
    .channels       = 12,		/* consumer-grade GPS */
    .probe_wakeup   = NULL,		/* no wakeup to be done before hunt */
    .probe_detect   = NULL,		/* no probe */
    .probe_subtype  = nmea_probe_subtype, /* probe for special types */
#ifdef ALLOW_RECONFIGURE
    .configurator   = NULL,		/* enable what we need */
#endif /* ALLOW_RECONFIGURE */
    .get_packet     = generic_get,	/* use generic packet getter */
    .parse_packet   = nmea_parse_input,	/* how to interpret a packet */
    .rtcm_writer    = pass_rtcm,	/* write RTCM data straight */
    .speed_switcher = NULL,		/* no speed switcher */
    .mode_switcher  = NULL,		/* no mode switcher */
    .rate_switcher  = NULL,		/* no sample-rate switcher */
    .cycle_chars    = -1,		/* not relevant, no rate switch */
#ifdef ALLOW_RECONFIGURE
    .revert	    = NULL,		/* no setting-reversion method */
#endif /* ALLOW_RECONFIGURE */
    .wrapup	    = NULL,		/* no wrapup */
    .cycle	    = 1,		/* updates every second */
};

#if defined(GARMIN_ENABLE) && defined(NMEA_ENABLE)
/**************************************************************************
 *
 * Garmin NMEA
 *
 **************************************************************************/

#ifdef ALLOW_RECONFIGURE
static void garmin_nmea_configurator(struct gps_device_t *session, unsigned int seq)
{
    /*
     * Receivers like the Garmin GPS-10 don't handle having having a lot of
     * probes shoved at them very well.
     */
    switch (seq) {
    case 0:
	/* reset some config, AutoFix, WGS84, PPS 
	 * Set the PPS pulse length to 40ms which leaves the Garmin 18-5hz 
         * with a 160ms low state.
         * NOTE: new PPS only takes effect after next power cycle
         */
	(void)nmea_send(session->gpsdata.gps_fd, "$PGRMC,A,,100,,,,,,A,,1,2,1,30");
	break;
    case 1:
	/* once a sec, no averaging, NMEA 2.3, WAAS */
	(void)nmea_send(session->gpsdata.gps_fd, "$PGRMC1,1,1,1,,,,2,W,N");
	break;
    case 2:
	/* get some more config info */
	(void)nmea_send(session->gpsdata.gps_fd, "$PGRMC1E");
	break;
    case 3:
	/* turn off all output except GGA */
	(void)nmea_send(session->gpsdata.gps_fd, "$PGRMO,,2");
	(void)nmea_send(session->gpsdata.gps_fd, "$PGRMO,GPGGA,1");
	break;
    case 4:
	/* enable GPGGA, GPGSA, GPGSV, GPRMC on Garmin serial GPS */
	(void)nmea_send(session->gpsdata.gps_fd, "$PGRMO,GPGSA,1");
	break;
    case 5:
	(void)nmea_send(session->gpsdata.gps_fd, "$PGRMO,GPGSV,1");
	break;
    case 6:
	(void)nmea_send(session->gpsdata.gps_fd, "$PGRMO,GPRMC,1");
	break;
    case 7:
	(void)nmea_send(session->gpsdata.gps_fd, "$PGRMO,PGRME,1");
	break;
    }
}
#endif /* ALLOW_RECONFIGURE */

static struct gps_type_t garmin = {
    .type_name      = "Garmin Serial",	/* full name of type */
    .trigger	    = "$PGRMC,",	/* Garmin private */
    .channels       = 12,		/* not used by this driver */
    .probe_wakeup   = NULL,		/* no wakeup to be done before hunt */
    .probe_detect   = NULL,		/* no probe */
    .probe_subtype  = NULL,		/* no further querying */
#ifdef ALLOW_RECONFIGURE
    .configurator   = garmin_nmea_configurator,/* enable what we need */
#endif /*ALLOW_RECONFIGURE */
    .get_packet     = generic_get,	/* use generic packet getter */
    .parse_packet   = nmea_parse_input,	/* how to interpret a packet */
    .rtcm_writer    = NULL,		/* some do, some don't, skip for now */
    .speed_switcher = NULL,		/* no speed switcher */
    .mode_switcher  = NULL,		/* no mode switcher */
    .rate_switcher  = NULL,		/* no sample-rate switcher */
    .cycle_chars    = -1,		/* not relevant, no rate switch */
#ifdef ALLOW_RECONFIGURE
    .revert	    = NULL,		/* no setting-reversion method */
#endif /*ALLOW_RECONFIGURE */
    .wrapup	    = NULL,		/* no wrapup */
    .cycle	    = 1,		/* updates every second */
};
#endif /* GARMIN_ENABLE && NMEA_ENABLE */

#ifdef ASHTECH_ENABLE
/**************************************************************************
 *
 * Ashtech (then Thales, now Magellan Professional) Receivers
 *
 **************************************************************************/

#ifdef ALLOW_RECONFIGURE
static void ashtech_configure(struct gps_device_t *session, unsigned int seq)
{
    if (seq == 0){
	/* turn WAAS on. can't hurt... */
	(void)nmea_send(session->gpsdata.gps_fd, "$PASHS,WAS,ON");
	/* reset to known output state */
	(void)nmea_send(session->gpsdata.gps_fd, "$PASHS,NME,ALL,A,OFF");
	/* then turn on some useful sentences */
#ifdef ASHTECH_NOTYET
	/* we could parse these, but they're oversize so they get dropped */
	(void)nmea_send(session->gpsdata.gps_fd, "$PASHS,NME,POS,A,ON");
	(void)nmea_send(session->gpsdata.gps_fd, "$PASHS,NME,SAT,A,ON");
#else
	(void)nmea_send(session->gpsdata.gps_fd, "$PASHS,NME,GGA,A,ON");
	(void)nmea_send(session->gpsdata.gps_fd, "$PASHS,NME,GSA,A,ON");
	(void)nmea_send(session->gpsdata.gps_fd, "$PASHS,NME,GSV,A,ON");
	(void)nmea_send(session->gpsdata.gps_fd, "$PASHS,NME,RMC,A,ON");
#endif
	(void)nmea_send(session->gpsdata.gps_fd, "$PASHS,NME,ZDA,A,ON");
    }
}
#endif /* ALLOW_RECONFIGURE */

static void ashtech_ping(struct gps_device_t *session)
{
	(void)nmea_send(session->gpsdata.gps_fd, "$PASHQ,RID");
}

static struct gps_type_t ashtech = {
    .type_name      = "Ashtech",	/* full name of type */
    .trigger	    = "$PASHR,RID,",	/* Ashtech receivers respond thus */
    .channels       = 24,		/* not used, GG24 has 24 channels */
    .probe_wakeup   = ashtech_ping,	/* no wakeup to be done before hunt */
    .probe_detect   = NULL,		/* no probe */
    .probe_subtype  = NULL,		/* to be sent unconditionally */
#ifdef ALLOW_RECONFIGURE
    .configurator   = ashtech_configure, /* change its sentence set */
#endif /* ALLOW_RECONFIGURE */
    .get_packet     = generic_get,	/* how to get a packet */
    .parse_packet   = nmea_parse_input,	/* how to interpret a packet */
    .rtcm_writer    = pass_rtcm,	/* write RTCM data straight */
    .speed_switcher = NULL,		/* no speed switcher */
    .mode_switcher  = NULL,		/* no mode switcher */
    .rate_switcher  = NULL,		/* no sample-rate switcher */
    .cycle_chars    = -1,		/* not relevant, no rate switch */
#ifdef ALLOW_RECONFIGURE
    .revert	    = NULL,		/* no setting-reversion method */
#endif /* ALLOW_RECONFIGURE */
    .wrapup	    = NULL,		/* no wrapup */
    .cycle	    = 1,		/* updates every second */
};
#endif /* ASHTECH_ENABLE */

#ifdef FV18_ENABLE
/**************************************************************************
 *
 * FV18 -- uses 2 stop bits, needs to be told to send GSAs
 *
 **************************************************************************/

#ifdef ALLOW_RECONFIGURE
static void fv18_configure(struct gps_device_t *session, unsigned int seq)
{
    /*
     * Tell an FV18 to send GSAs so we'll know if 3D is accurate.
     * Suppress GLL and VTG.  Enable ZDA so dates will be accurate for replay.
     */
    if (seq == 0)
	(void)nmea_send(session->gpsdata.gps_fd,
		    "$PFEC,GPint,GSA01,DTM00,ZDA01,RMC01,GLL00,VTG00,GSV05");
}
#endif /* ALLOW_RECONFIGURE */

static struct gps_type_t fv18 = {
    .type_name      = "San Jose Navigation FV18",	/* full name of type */
    .trigger	    = "$PFEC,GPint,",	/* FV18s should echo the probe */
    .channels       = 12,		/* not used by this driver */
    .probe_wakeup   = NULL,		/* no wakeup to be done before hunt */
    .probe_detect   = NULL,		/* no probe */
    .probe_subtype  = NULL,		/* to be sent unconditionally */
#ifdef ALLOW_RECONFIGURE
    .configurator   = fv18_configure,	/* change its sentence set */
#endif /* ALLOW_RECONFIGURE */
    .get_packet     = generic_get,	/* how to get a packet */
    .parse_packet   = nmea_parse_input,	/* how to interpret a packet */
    .rtcm_writer    = pass_rtcm,	/* write RTCM data straight */
    .speed_switcher = NULL,		/* no speed switcher */
    .mode_switcher  = NULL,		/* no mode switcher */
    .rate_switcher  = NULL,		/* no sample-rate switcher */
    .cycle_chars    = -1,		/* not relevant, no rate switch */
#ifdef ALLOW_RECONFIGURE
    .revert	    = NULL,		/* no setting-reversion method */
#endif /* ALLOW_RECONFIGURE */
    .wrapup	    = NULL,		/* no wrapup */
    .cycle	    = 1,		/* updates every second */
};
#endif /* FV18_ENABLE */

#ifdef GPSCLOCK_ENABLE
/**************************************************************************
 *
 * Furuno Electric GPSClock (GH-79L4)
 *
 **************************************************************************/

/*
 * Based on http://www.tecsys.de/fileadmin/user_upload/pdf/gh79_1an_intant.pdf
 */

static void gpsclock_probe_subtype(struct gps_device_t *session, unsigned int seq)
{
    /*
     * Michael St. Laurent <mikes@hartwellcorp.com> reports that you have to
     * ignore the trailing PPS edge when extracting time from this chip.
     */
    if (seq == 0) {
	gpsd_report(LOG_INF, "PPS trailing edge will be ignored");
	session->driver.nmea.ignore_trailing_edge = true;
    }
}

static struct gps_type_t gpsclock = {
    .type_name      = "Furuno Electric GH-79L4",	/* full name of type */
    .trigger	    = "$PFEC,GPssd",	/* GPSclock should echo the probe */
    .channels       = 12,		/* not used by this driver */
    .probe_wakeup   = NULL,		/* no wakeup to be done before hunt */
    .probe_detect   = NULL,		/* no probe */
    .probe_subtype  = gpsclock_probe_subtype, /* to be sent unconditionally */
#ifdef ALLOW_RECONFIGURE
    .configurator   = NULL,		/* change its sentence set */
#endif /* ALLOW_RECONFIGURE */
    .get_packet     = generic_get,	/* how to get a packet */
    .parse_packet   = nmea_parse_input,	/* how to interpret a packet */
    .rtcm_writer    = pass_rtcm,	/* write RTCM data straight */
    .speed_switcher = NULL,		/* no speed switcher */
    .mode_switcher  = NULL,		/* no mode switcher */
    .rate_switcher  = NULL,		/* sample rate is fixed */
    .cycle_chars    = -1,		/* sample rate is fixed */
#ifdef ALLOW_RECONFIGURE
    .revert	    = NULL,		/* no setting-reversion method */
#endif /* ALLOW_RECONFIGURE */
    .wrapup	    = NULL,		/* no wrapup */
    .cycle	    = 1,		/* updates every second */
};
#endif /* GPSCLOCK_ENABLE */

#ifdef TRIPMATE_ENABLE
/**************************************************************************
 *
 * TripMate -- extended NMEA, gets faster fix when primed with lat/long/time
 *
 **************************************************************************/

/*
 * Some technical FAQs on the TripMate:
 * http://vancouver-webpages.com/pub/peter/tripmate.faq
 * http://www.asahi-net.or.jp/~KN6Y-GTU/tripmate/trmfaqe.html
 * The TripMate was discontinued sometime before November 1998
 * and was replaced by the Zodiac EarthMate.
 */

static void tripmate_probe_subtype(struct gps_device_t *session, unsigned int seq)
{
    /* TripMate requires this response to the ASTRAL it sends at boot time */
    if (seq == 0)
	(void)nmea_send(session->gpsdata.gps_fd, "$IIGPQ,ASTRAL");
}

#ifdef ALLOW_RECONFIGURE
static void tripmate_configurator(struct gps_device_t *session, unsigned int seq)
{
    /* stop it sending PRWIZCH */
    if (seq == 0)
	(void)nmea_send(session->gpsdata.gps_fd, "$PRWIILOG,ZCH,V,,");
}
#endif /* ALLOW_RECONFIGURE */

static struct gps_type_t tripmate = {
    .type_name     = "Delorme TripMate",	/* full name of type */
    .trigger       ="ASTRAL",			/* tells us to switch */
    .channels      = 12,			/* consumer-grade GPS */
    .probe_wakeup  = NULL,			/* no wakeup before hunt */
    .probe_detect  = NULL,			/* no probe */
    .probe_subtype = tripmate_probe_subtype,	/* send unconditionally */
#ifdef ALLOW_RECONFIGURE
    .configurator  = tripmate_configurator,	/* send unconditionally */
#endif /* ALLOW_RECONFIGURE */
    .get_packet    = generic_get,		/* how to get a packet */
    .parse_packet  = nmea_parse_input,		/* how to interpret a packet */
    .rtcm_writer   = pass_rtcm,			/* send RTCM data straight */
    .speed_switcher= NULL,			/* no speed switcher */
    .mode_switcher = NULL,			/* no mode switcher */
    .rate_switcher = NULL,			/* no sample-rate switcher */
    .cycle_chars   = -1,			/* no rate switch */
#ifdef ALLOW_RECONFIGURE
    .revert	   = NULL,			/* no setting-reversion method */
#endif /* ALLOW_RECONFIGURE */
    .wrapup	   = NULL,			/* no wrapup */
    .cycle	   = 1,				/* updates every second */
};
#endif /* TRIPMATE_ENABLE */

#ifdef EARTHMATE_ENABLE
/**************************************************************************
 *
 * Zodiac EarthMate textual mode
 *
 * Note: This is the pre-2003 version using Zodiac binary protocol.
 * It has been replaced with a design that uses a SiRF chipset.
 *
 **************************************************************************/

static struct gps_type_t earthmate;

/*
 * There is a good HOWTO at <http://www.hamhud.net/ka9mva/earthmate.htm>.
 */

static void earthmate_close(struct gps_device_t *session)
{
    /*@i@*/session->device_type = &earthmate;
}

static void earthmate_probe_subtype(struct gps_device_t *session, unsigned int seq)
{
    if (seq == 0) {
	(void)gpsd_write(session, "EARTHA\r\n", 8);
	(void)usleep(10000);
	/*@i@*/session->device_type = &zodiac_binary;
	zodiac_binary.wrapup = earthmate_close;
	if (zodiac_binary.probe_subtype) zodiac_binary.probe_subtype(session, seq);
    }
}

/*@ -redef @*/
static struct gps_type_t earthmate = {
    .type_name     = "Delorme EarthMate (pre-2003, Zodiac chipset)",
    .trigger       = "EARTHA",			/* Earthmate trigger string */
    .channels      = 12,			/* not used by NMEA parser */
    .probe_wakeup  = NULL,			/* no wakeup to be done before hunt */
    .probe_detect  = NULL,			/* no probe */
    .probe_subtype = earthmate_probe_subtype,	/* switch us to Zodiac mode */
#ifdef ALLOW_RECONFIGURE
    .configurator  = NULL,			/* no configuration here */
#endif /* ALLOW_RECONFIGURE */
    .get_packet    = generic_get,		/* how to get a packet */
    .parse_packet  = nmea_parse_input,		/* how to interpret a packet */
    .rtcm_writer   = NULL,			/* don't send RTCM data */
    .speed_switcher= NULL,			/* no speed switcher */
    .mode_switcher = NULL,			/* no mode switcher */
    .rate_switcher = NULL,			/* no sample-rate switcher */
    .cycle_chars   = -1,			/* no rate switch */
#ifdef ALLOW_RECONFIGURE
    .revert	   = NULL,			/* no setting-reversion method */
#endif /* ALLOW_RECONFIGURE */
    .wrapup	   = NULL,			/* no wrapup code */
    .cycle	   = 1,				/* updates every second */
};
/*@ -redef @*/
#endif /* EARTHMATE_ENABLE */

#endif /* NMEA_ENABLE */

#ifdef TNT_ENABLE
/**************************************************************************
 * True North Technologies - Revolution 2X Digital compass
 *
 * More info: http://www.tntc.com/
 *
 * This is a digital compass which uses magnetometers to measure the
 * strength of the earth's magnetic field. Based on these measurements
 * it provides a compass heading using NMEA formatted output strings.
 * This is useful to supplement the heading provided by another GPS
 * unit. A GPS heading is unreliable at slow speed or no speed.
 *
 **************************************************************************/

enum {
#include "packet_states.h"
};

static void tnt_add_checksum(char *sentence)
{
    unsigned char sum = '\0';
    char c, *p = sentence;

    if (*p == '@') {
	p++;
    } else {
	gpsd_report(LOG_ERROR, "Bad TNT sentence: '%s'\n", sentence);
    }
    while ( ((c = *p) != '*') && (c != '\0')) {
	sum ^= c;
	p++;
    }
    *p++ = '*';
    /*@i@*/snprintf(p, 4, "%02X\r\n", sum);
}

static int tnt_send(int fd, const char *fmt, ... )
{
    int status;
    char buf[BUFSIZ];
    va_list ap;

    va_start(ap, fmt) ;
    (void)vsnprintf(buf, sizeof(buf)-5, fmt, ap);
    va_end(ap);
    strlcat(buf, "*", BUFSIZ);
    tnt_add_checksum(buf);
    status = (int)write(fd, buf, strlen(buf));
    tcdrain(fd);
    if (status == (int)strlen(buf)) {
	gpsd_report(LOG_IO, "=> GPS: %s\n", buf);
	return status;
    } else {
	gpsd_report(LOG_WARN, "=> GPS: %s FAILED\n", buf);
	return -1;
    }
}

#define TNT_SNIFF_RETRIES       100
/*
 * The True North compass won't start talking
 * unless you ask it to. So to identify it we
 * need to query for its ID string.
 */
static int tnt_packet_sniff(struct gps_device_t *session)
{
    unsigned int n, count = 0;

    gpsd_report(LOG_RAW, "tnt_packet_sniff begins\n");
    for (n = 0; n < TNT_SNIFF_RETRIES; n++)
    {
      count = 0;
      (void)tnt_send(session->gpsdata.gps_fd, "@X?");
      if (ioctl(session->gpsdata.gps_fd, FIONREAD, &count) < 0)
	  return BAD_PACKET;
      if (count == 0) {
	  //int delay = 10000000000.0 / session->gpsdata.baudrate;
	  //gpsd_report(LOG_RAW, "usleep(%d)\n", delay);
	  //usleep(delay);
	  gpsd_report(LOG_RAW, "sleep(1)\n");
	  (void)sleep(1);
      } else if (generic_get(session) >= 0) {
	if((session->packet.type == NMEA_PACKET)&&(session->packet.state == NMEA_RECOGNIZED))
	{
	  gpsd_report(LOG_RAW, "tnt_packet_sniff returns %d\n",session->packet.type);
	  return session->packet.type;
	}
      }
    }

    gpsd_report(LOG_RAW, "tnt_packet_sniff found no packet\n");
    return BAD_PACKET;
}

static void tnt_probe_subtype(struct gps_device_t *session, unsigned int seq UNUSED)
{
  // Send codes to start the flow of data
  //tnt_send(session->gpsdata.gps_fd, "@BA?"); // Query current rate
  //tnt_send(session->gpsdata.gps_fd, "@BA=8"); // Start HTM packet at 1Hz
  /*
   * Sending this twice seems to make it more reliable!!
   * I think it gets the input on the unit synced up.
   */
  (void)tnt_send(session->gpsdata.gps_fd, "@BA=15"); // Start HTM packet at 1200 per minute
  (void)tnt_send(session->gpsdata.gps_fd, "@BA=15"); // Start HTM packet at 1200 per minute
}

static bool tnt_probe(struct gps_device_t *session)
{
  unsigned int *ip;
#ifdef FIXED_PORT_SPEED
    /* just the one fixed port speed... */
    static unsigned int rates[] = {FIXED_PORT_SPEED};
#else /* FIXED_PORT_SPEED not defined */
  /* The supported baud rates */
  static unsigned int rates[] = {38400, 19200, 2400, 4800, 9600 };
#endif /* FIXED_PORT_SPEED defined */

  gpsd_report(LOG_PROG, "Probing TrueNorth Compass\n");

  /*
   * Only block until we get at least one character, whatever the
   * third arg of read(2) says.
   */
  /*@ ignore @*/
  memset(session->ttyset.c_cc,0,sizeof(session->ttyset.c_cc));
  session->ttyset.c_cc[VMIN] = 1;
  /*@ end @*/

  session->ttyset.c_cflag &= ~(PARENB | PARODD | CRTSCTS);
  session->ttyset.c_cflag |= CREAD | CLOCAL;
  session->ttyset.c_iflag = session->ttyset.c_oflag = session->ttyset.c_lflag = (tcflag_t) 0;

  session->baudindex = 0;
  for (ip = rates; ip < rates + sizeof(rates)/sizeof(rates[0]); ip++)
      if (ip == rates || *ip != rates[0])
      {
	  gpsd_report(LOG_PROG, "hunting at speed %d\n", *ip);
	  gpsd_set_speed(session, *ip, 'N',1);
	  if (tnt_packet_sniff(session) != BAD_PACKET)
	      return true;
      }
  return false;
}

struct gps_type_t trueNorth = {
    .type_name      = "True North",	/* full name of type */
    .trigger	    = " TNT1500",
    .channels       = 0,		/* not an actual GPS at all */
    .probe_wakeup   = NULL,		/* this will become a real method */
    .probe_detect   = tnt_probe,	/* probe by sending ID query */
    .probe_subtype  = tnt_probe_subtype,/* probe for True North Digital Compass */
#ifdef ALLOW_RECONFIGURE
    .configurator   = NULL,		/* no setting changes */
#endif /* ALLOW_RECONFIGURE */
    .get_packet     = generic_get,	/* how to get a packet */
    .parse_packet   = nmea_parse_input,	/* how to interpret a packet */
    .rtcm_writer    = NULL,		/* Don't send */
    .speed_switcher = NULL,		/* no speed switcher */
    .mode_switcher  = NULL,		/* no mode switcher */
    .rate_switcher  = NULL,		/* no wrapup */
    .cycle_chars    = -1,		/* not relevant, no rate switch */
#ifdef ALLOW_RECONFIGURE
    .revert	    = NULL,		/* no setting-reversion method */
#endif /* ALLOW_RECONFIGURE */
    .wrapup	    = NULL,		/* no wrapup */
    .cycle	    = 20,		/* updates per second */
};
#endif
#ifdef RTCM104_ENABLE
/**************************************************************************
 *
 * RTCM-104, used for broadcasting DGPS corrections and by DGPS radios
 *
 **************************************************************************/

static gps_mask_t rtcm104_analyze(struct gps_device_t *session)
{
    rtcm_unpack(&session->gpsdata.rtcm, (char *)session->packet.isgps.buf);
    gpsd_report(LOG_RAW, "RTCM packet type 0x%02x length %d words: %s\n",
		session->gpsdata.rtcm.type,
		session->gpsdata.rtcm.length+2,
		gpsd_hexdump(session->packet.isgps.buf, (session->gpsdata.rtcm.length+2)*sizeof(isgps30bits_t)));
    return RTCM_SET;
}

static struct gps_type_t rtcm104 = {
    .type_name     = "RTCM104",		/* full name of type */
    .trigger       = NULL,		/* no recognition string */
    .channels      = 0,			/* not used */
    .probe_wakeup  = NULL,		/* no wakeup to be done before hunt */
    .probe_detect  = NULL,		/* no probe */
    .probe_subtype = NULL,		/* no subtypes */
#ifdef ALLOW_RECONFIGURE
    .configurator  = NULL,		/* no configurator */
#endif /* ALLOW_RECONFIGURE */
    .get_packet    = generic_get,	/* how to get a packet */
    .parse_packet  = rtcm104_analyze,	/*  */
    .rtcm_writer   = NULL,		/* don't send RTCM data,  */
    .speed_switcher= NULL,		/* no speed switcher */
    .mode_switcher = NULL,		/* no mode switcher */
    .rate_switcher = NULL,		/* no sample-rate switcher */
    .cycle_chars   = -1,		/* not relevant, no rate switch */
#ifdef ALLOW_RECONFIGURE
    .revert	   = NULL,		/* no setting-reversion method */
#endif /* ALLOW_RECONFIGURE */
    .wrapup	   = NULL,		/* no wrapup code */
    .cycle	   = 1,			/* updates every second */
};
#endif /* RTCM104_ENABLE */

#ifdef GARMINTXT_ENABLE
/**************************************************************************
 *
 * Garmin Simple Text protocol
 *
 **************************************************************************/

static gps_mask_t garmintxt_parse_input(struct gps_device_t *session)
{
    //gpsd_report(LOG_PROG, "Garmin Simple Text packet\n");
    return garmintxt_parse(session);
}


static struct gps_type_t garmintxt = {
    .type_name     = "Garmin Simple Text",		/* full name of type */
    .trigger       = NULL,		/* no recognition string */
    .channels      = 0,			/* not used */
    .probe_wakeup  = NULL,		/* no wakeup to be done before hunt */
    .probe_detect  = NULL,		/* no probe */
    .probe_subtype = NULL,		/* no subtypes */
#ifdef ALLOW_RECONFIGURE
    .configurator  = NULL,		/* no configurator */
#endif /* ALLOW_RECONFIGURE */
    .get_packet    = generic_get,	/* how to get a packet */
    .parse_packet  = garmintxt_parse_input,	/*  */
    .rtcm_writer   = NULL,		/* don't send RTCM data,  */
    .speed_switcher= NULL,		/* no speed switcher */
    .mode_switcher = NULL,		/* no mode switcher */
    .rate_switcher = NULL,		/* no sample-rate switcher */
    .cycle_chars   = -1,		/* not relevant, no rate switch */
#ifdef ALLOW_RECONFIGURE
    .revert	   = NULL,		/* no setting-reversion method */
#endif /* ALLOW_RECONFIGURE */
    .wrapup	   = NULL,		/* no wrapup code */
    .cycle	   = 1,			/* updates every second */
};
#endif /* GARMINTXT_ENABLE */

extern struct gps_type_t garmin_usb_binary, garmin_ser_binary;
extern struct gps_type_t sirf_binary, tsip_binary;
extern struct gps_type_t evermore_binary, italk_binary;
extern struct gps_type_t navcom_binary;

/*@ -nullassign @*/
/* the point of this rigamarole is to not have to export a table size */
static struct gps_type_t *gpsd_driver_array[] = {
#ifdef NMEA_ENABLE
    &nmea,
#ifdef ASHTECH_ENABLE
    &ashtech,
#endif /* ASHTECHV18_ENABLE */
#ifdef FV18_ENABLE
    &fv18,
#endif /* FV18_ENABLE */
#ifdef GPSCLOCK_ENABLE
    &gpsclock,
#endif /* GPSCLOCK_ENABLE */
#ifdef GARMIN_ENABLE
    &garmin,
#endif /* GARMIN_ENABLE */
#ifdef TRIPMATE_ENABLE
    &tripmate,
#endif /* TRIPMATE_ENABLE */
#ifdef EARTHMATE_ENABLE
    &earthmate,
#endif /* EARTHMATE_ENABLE */
#endif /* NMEA_ENABLE */
#ifdef ZODIAC_ENABLE
    &zodiac_binary,
#endif /* ZODIAC_ENABLE */
#ifdef NAVCOM_ENABLE
    &navcom_binary,
#endif /* NAVCOM_ENABLE */
#ifdef UBX_ENABLE
    &ubx_binary,
#endif /* UBX_ENABLE */
#ifdef GARMIN_ENABLE
    &garmin_usb_binary,
    &garmin_ser_binary,
#endif /* GARMIN_ENABLE */
#ifdef SIRF_ENABLE
    &sirf_binary,
#endif /* SIRF_ENABLE */
#ifdef TSIP_ENABLE
    &tsip_binary,
#endif /* TSIP_ENABLE */
#ifdef TNT_ENABLE
    &trueNorth,
#endif /* TSIP_ENABLE */
#ifdef EVERMORE_ENABLE
    &evermore_binary,
#endif /* EVERMORE_ENABLE */
#ifdef ITRAX_ENABLE
    &italk_binary,
#endif /* ITRAX_ENABLE */
#ifdef RTCM104_ENABLE
    &rtcm104,
#endif /* RTCM104_ENABLE */
#ifdef GARMINTXT_ENABLE
    &garmintxt,
#endif /* GARMINTXT_ENABLE */
    NULL,
};
/*@ +nullassign @*/
struct gps_type_t **gpsd_drivers = &gpsd_driver_array[0];
