#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <sys/time.h>
#include <stdarg.h>

#include "gpsd.h"

extern struct gps_type_t zodiac_binary;

#if defined(NMEA_ENABLE) || defined(SIRFII_ENABLL) || defined(EVERMORE_ENABLE)  || defined(ITALK_ENABLE) 
ssize_t pass_rtcm(struct gps_device_t *session, char *buf, size_t rtcmbytes)
/* most GPSes take their RTCM corrections straight up */
{
    return write(session->gpsdata.gps_fd, buf, rtcmbytes);
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
    if (session->packet_type == SIRF_PACKET) {
	gpsd_report(2, "SiRF packet seen when NMEA expected.\n");
#ifdef SIRFII_ENABLE
	return sirf_parse(session, session->outbuffer, session->outbuflen);
#else
	return 0;
#endif /* SIRFII_ENABLE */
    } else if (session->packet_type == EVERMORE_PACKET) {
	gpsd_report(2, "EverMore packet seen when NMEA expected.\n");
#ifdef EVERMORE_ENABLE
	/* we might never see a $PEMT, have this as a backstop */
	(void)gpsd_switch_driver(session, "EverMore binary");
	return evermore_parse(session, session->outbuffer, session->outbuflen);
#else
	return 0;
#endif /* EVERMORE_ENABLE */
    } else if (session->packet_type == NMEA_PACKET) {
	gps_mask_t st = 0;
	gpsd_report(2, "<= GPS: %s", session->outbuffer);
	if ((st=nmea_parse((char *)session->outbuffer, session))==0) {
#ifdef NON_NMEA_ENABLE
	    struct gps_type_t **dp;

	    /* maybe this is a trigger string for a driver we know about? */
	    for (dp = gpsd_drivers; *dp; dp++) {
		char	*trigger = (*dp)->trigger;

		if (trigger!=NULL && strncmp((char *)session->outbuffer, trigger, strlen(trigger))==0 && isatty(session->gpsdata.gps_fd)!=0) {
		    gpsd_report(1, "found %s.\n", trigger);
		    (void)gpsd_switch_driver(session, (*dp)->typename);
		    return 1;
		}
	    }
#endif /* NON_NMEA_ENABLE */
	    gpsd_report(1, "unknown sentence: \"%s\"\n", session->outbuffer);
	}
#ifdef NTPSHM_ENABLE
	if ((st & TIME_SET) != 0)
	    /* this magic number is derived from observation */
	    (void)ntpshm_put(session, session->gpsdata.fix.time + 0.675);
#endif /* NTPSHM_ENABLE */
	return st;
    } else
	return 0;
}

static void nmea_initializer(struct gps_device_t *session)
{
#ifdef NMEA_ENABLE
    /*
     * Tell an FV18 to send GSAs so we'll know if 3D is accurate.
     * Suppress GLL and VTG.  Enable ZDA so dates will be accurate for replay.
     */
#define FV18_PROBE	"$PFEC,GPint,GSA01,DTM00,ZDA01,RMC01,GLL00,VTG00,GSV05"
    (void)nmea_send(session->gpsdata.gps_fd, FV18_PROBE);
    /* Sony CXD2951 chips: +GGA, -GLL, +GSA, +GSV, +RMC, -VTG, +ZDA, -PSGSA */
    (void)nmea_send(session->gpsdata.gps_fd, "@NC10151010");
    /* enable GPZDA on a Motorola Oncore GT+ */
    (void)nmea_send(session->gpsdata.gps_fd, "$PMOTG,ZDA,1");
    /* probe for Garmin serial GPS */
    (void)nmea_send(session->gpsdata.gps_fd, "$PGRMCE");
#endif /* NMEA_ENABLE */
#ifdef SIRFII_ENABLE
    /* probe for SiRF-II */
    (void)nmea_send(session->gpsdata.gps_fd, "$PSRF105,1");
#endif /* SIRFII_ENABLE */
#ifdef ITRAX_ENABLE
    /* probe for iTrax, looking for "OK" */
    (void)nmea_send(session->gpsdata.gps_fd, "$PFST");
#endif /* ITRAX_ENABLE */
#ifdef EVERMORE_ENABLE
    /* probe for EverMore by trying to read the LogConfig */
    (void)gpsd_write(session, "\x10\x02\x06\x8d\x00\x01\x00\x8e\x10\x03", 10);
#endif /* EVERMORE_ENABLE */
}

static struct gps_type_t nmea = {
    .typename       = "Generic NMEA",	/* full name of type */
    .trigger        = NULL,		/* it's the default */
    .channels       = 12,		/* consumer-grade GPS */
    .probe          = NULL,		/* no probe */
    .initializer    = nmea_initializer,	/* probe for special types */
    .get_packet     = packet_get,		/* use generic packet getter */
    .parse_packet   = nmea_parse_input,	/* how to interpret a packet */
    .rtcm_writer    = pass_rtcm,	/* write RTCM data straight */
    .speed_switcher = NULL,		/* no speed switcher */
    .mode_switcher  = NULL,		/* no mode switcher */
    .rate_switcher  = NULL,		/* no sample-rate switcher */
    .cycle_chars    = -1,		/* not relevant, no rate switch */
    .wrapup         = NULL,		/* no wrapup */
    .cycle          = 1,		/* updates every second */
};

static void garmin_initializer(struct gps_device_t *session)
{
#ifdef NMEA_ENABLE
    /* reset some config, AutoFix, WGS84, PPS */
    (void)nmea_send(session->gpsdata.gps_fd, "$PGRMC,A,,100,,,,,,A,,1,2,4,30");
    /* once a sec, no averaging, NMEA 2.3, WAAS */
    (void)nmea_send(session->gpsdata.gps_fd, "$PGRMC1,1,1,2,,,,2,W,N");
    /* get some more config info */
    (void)nmea_send(session->gpsdata.gps_fd, "$PGRMC1E");
    /* turn off all output */
    (void)nmea_send(session->gpsdata.gps_fd, "$PGRMO,,2");
    /* enable GPGGA, GPGSA, GPGSV, GPRMC on Garmin serial GPS */
    (void)nmea_send(session->gpsdata.gps_fd, "$PGRMO,GPGGA,1");
    (void)nmea_send(session->gpsdata.gps_fd, "$PGRMO,GPGSA,1");
    (void)nmea_send(session->gpsdata.gps_fd, "$PGRMO,GPGSV,1");
    (void)nmea_send(session->gpsdata.gps_fd, "$PGRMO,GPRMC,1");
#endif /* NMEA_ENABLE */
}

static struct gps_type_t garmin = {
    .typename       = "Garmin Serial",	/* full name of type */
    .trigger        = "$PGRMC",		/* Garmin private */
    .channels       = 12,		/* consumer-grade GPS */
    .probe          = NULL,		/* no probe */
    .initializer    = garmin_initializer,/* probe for special types */
    .get_packet     = packet_get,	/* use generic packet getter */
    .parse_packet   = nmea_parse_input,	/* how to interpret a packet */
    .rtcm_writer    = NULL,		/* some do, some don't, skip for now */
    .speed_switcher = NULL,		/* no speed switcher */
    .mode_switcher  = NULL,		/* no mode switcher */
    .rate_switcher  = NULL,		/* no sample-rate switcher */
    .cycle_chars    = -1,		/* not relevant, no rate switch */
    .wrapup         = NULL,		/* no wrapup */
    .cycle          = 1,		/* updates every second */
};

#if FV18_ENABLE
/**************************************************************************
 *
 * FV18 -- uses 2 stop bits, needs to be told to send GSAs
 *
 **************************************************************************/

static struct gps_type_t fv18 = {
    .typename       = "San Jose Navigation FV18",	/* full name of type */
    .trigger        = FV18_PROBE,	/* FV18s should echo the probe */
    .channels       = 12,		/* consumer-grade GPS */
    .probe          = NULL,		/* mo probe */
    .initializer    = NULL,		/* to be sent unconditionally */
    .get_packet     = packet_get,	/* how to get a packet */
    .parse_packet   = nmea_parse_input,	/* how to interpret a packet */
    .rtcm_writer    = pass_rtcm,	/* write RTCM data straight */
    .speed_switcher = NULL,		/* no speed switcher */
    .mode_switcher  = NULL,		/* no mode switcher */
    .rate_switcher  = NULL,		/* no sample-rate switcher */
    .cycle_chars    = -1,		/* not relevant, no rate switch */
    .wrapup         = NULL,		/* no wrapup */
    .cycle          = 1,		/* updates every second */
};
#endif /* FV18_ENABLE */

/**************************************************************************
 *
 * SiRF-II NMEA
 *
 * This NMEA -mode driver is a fallback in case the SiRF chipset has
 * firmware too old for binary to be useful, or we're not compiling in
 * the SiRF binary driver at all.
 *
 **************************************************************************/

static void sirf_initializer(struct gps_device_t *session)
{
    /* (void)nmea_send(session->gpsdata.gps_fd, "$PSRF105,0"); */
    (void)nmea_send(session->gpsdata.gps_fd, "$PSRF105,0");
    (void)nmea_send(session->gpsdata.gps_fd, "$PSRF103,05,00,00,01"); /* no VTG */
    (void)nmea_send(session->gpsdata.gps_fd, "$PSRF103,01,00,00,01"); /* no GLL */
}

static bool sirf_switcher(struct gps_device_t *session, int nmea, unsigned int speed) 
/* switch GPS to specified mode at 8N1, optionally to binary */
{
    if (nmea_send(session->gpsdata.gps_fd, "$PSRF100,%d,%d,8,1,0", nmea,speed) < 0)
	return false;
    return true;
}

static bool sirf_speed(struct gps_device_t *session, unsigned int speed)
/* change the baud rate, remaining in SiRF NMWA mode */
{
    return sirf_switcher(session, 1, speed);
}

static void sirf_mode(struct gps_device_t *session, int mode)
/* change mode to SiRF binary, speed unchanged */
{
    if (mode == 1) {
	(void)gpsd_switch_driver(session, "SiRF-II binary");
	session->gpsdata.driver_mode = (unsigned int)sirf_switcher(session, 0, session->gpsdata.baudrate);
    } else
	session->gpsdata.driver_mode = 0;
}

static struct gps_type_t sirfII_nmea = {
    .typename      = "SiRF-II NMEA",	/* full name of type */
#ifndef SIRFII_ENABLE
    .trigger       = "$Ack Input105.",	/* expected response to SiRF PSRF105 */
#else
    .trigger       = NULL,		/* let the binary driver have it */
#endif /* SIRFII_ENABLE */
    .channels      = 12,		/* consumer-grade GPS */
    .probe         = NULL,		/* no probe */
    .initializer   = sirf_initializer,	/* turn off debugging messages */
    .get_packet    = packet_get,	/* how to get a packet */
    .parse_packet  = nmea_parse_input,	/* how to interpret a packet */
    .rtcm_writer   = pass_rtcm,		/* write RTCM data straight */
    .speed_switcher= sirf_speed,	/* we can change speeds */
    .mode_switcher = sirf_mode,		/* there's a mode switch */
    .rate_switcher = NULL,		/* no sample-rate switcher */
    .cycle_chars   = -1,		/* not relevant, no rate switch */
    .wrapup         = NULL,		/* no wrapup */
    .cycle          = 1,		/* updates every second */
};

#if TRIPMATE_ENABLE
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

static void tripmate_initializer(struct gps_device_t *session)
{
    /* TripMate requires this response to the ASTRAL it sends at boot time */
    (void)nmea_send(session->gpsdata.gps_fd, "$IIGPQ,ASTRAL");
    /* stop it sending PRWIZCH */
    (void)nmea_send(session->gpsdata.gps_fd, "$PRWIILOG,ZCH,V,,");
}

static struct gps_type_t tripmate = {
    .typename      = "Delorme TripMate",	/* full name of type */
    .trigger       ="ASTRAL",			/* tells us to switch */
    .channels      = 12,			/* consumer-grade GPS */
    .probe         = NULL,			/* no probe */
    .initializer   = tripmate_initializer,	/* send unconfitionally */
    .get_packet    = packet_get,		/* how to get a packet */
    .parse_packet  = nmea_parse_input,		/* how to interpret a packet */
    .rtcm_writer   = pass_rtcm,			/* send RTCM data straight */
    .speed_switcher= NULL,			/* no speed switcher */
    .mode_switcher = NULL,			/* no mode switcher */
    .rate_switcher = NULL,			/* no sample-rate switcher */
    .cycle_chars   = -1,			/* no rate switch */
    .wrapup         = NULL,			/* no wrapup */
    .cycle          = 1,			/* updates every second */
};
#endif /* TRIPMATE_ENABLE */

#ifdef EARTHMATE_ENABLE
/**************************************************************************
 *
 * Zodiac EarthMate textual mode
 *
 * Note: This is the pre-2003 version using Zodiac binary protocol.
 * It has been replaced with a design that uses a SiRF-II chipset.
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

static void earthmate_initializer(struct gps_device_t *session)
{
    (void)write(session->gpsdata.gps_fd, "EARTHA\r\n", 8);
    (void)usleep(10000);
    /*@i@*/session->device_type = &zodiac_binary;
    zodiac_binary.wrapup = earthmate_close;
    if (zodiac_binary.initializer) zodiac_binary.initializer(session);
}

/*@ -redef @*/
static struct gps_type_t earthmate = {
    .typename      = "Delorme EarthMate (pre-2003, Zodiac chipset)",
    .trigger       = "EARTHA",			/* Earthmate trigger string */
    .channels      = 12,			/* consumer-grade GPS */
    .probe         = NULL,			/* no probe */
    .initializer   = earthmate_initializer,	/* switch us to Zodiac mode */
    .get_packet    = packet_get,		/* how to get a packet */
    .parse_packet  = nmea_parse_input,		/* how to interpret a packet */
    .rtcm_writer   = NULL,			/* don't send RTCM data */
    .speed_switcher= NULL,			/* no speed switcher */
    .mode_switcher = NULL,			/* no mode switcher */
    .rate_switcher = NULL,			/* no sample-rate switcher */
    .cycle_chars   = -1,			/* no rate switch */
    .wrapup         = NULL,			/* no wrapup code */
    .cycle          = 1,			/* updates every second */
};
/*@ -redef @*/
#endif /* EARTHMATE_ENABLE */


#ifdef ITRAX_ENABLE
/**************************************************************************
 *
 * The NMEA mode of the iTrax chipset, as used in the FastTrax and others.
 *
 * As described by v1.31 of the NMEA Protocol Specification for the
 * iTrax02 Evaluation Kit, 2003-06-12.
 * v1.18 of the  manual, 2002-19-6, describes effectively
 * the same protocol, but without ZDA.
 *
 **************************************************************************/

/*
 * Enable GGA=0x2000, RMC=0x8000, GSA=0x0002, GSV=0x0001, ZDA=0x0004.
 * Disable GLL=0x1000, VTG=0x4000, FOM=0x0020, PPS=0x0010.
 * This is 82+75+67+(3*60)+34 = 438 characters 
 * 
 * 1200   => at most 1 fix per 4 seconds
 * 2400   => at most 1 fix per 2 seconds
 * 4800   => at most 1 fix per 1 seconds
 * 9600   => at most 2 fixes per second
 * 19200  => at most 4 fixes per second
 * 57600  => at most 13 fixes per second
 * 115200 => at most 26 fixes per second
 *
 * We'd use FOM, but they don't specify a confidence interval.
 */
#define ITRAX_MODESTRING	"$PFST,NMEA,A007,%d\r\n"

static int literal_send(int fd, const char *fmt, ... )
/* ship a raw command to the GPS */
{
    int status;
    char buf[BUFSIZ];
    va_list ap;

    va_start(ap, fmt) ;
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    status = (int)write(fd, buf, strlen(buf));
    if (status == (int)strlen(buf)) {
	gpsd_report(2, "=> GPS: %s\n", buf);
	return status;
    } else {
	gpsd_report(2, "=> GPS: %s FAILED\n", buf);
	return -1;
    }
}

static void itrax_initializer(struct gps_device_t *session)
/* start navigation and synchronous mode */
{
    /* initialize GPS clock with current system time */ 
    struct tm when;
    double integral, fractional;
    time_t intfixtime;
    char buf[31], frac[6];
    fractional = modf(timestamp(), &integral);
    intfixtime = (time_t)integral;
    (void)gmtime_r(&intfixtime, &when);
    (void)strftime(buf, sizeof(buf), "$PFST,INITAID,%H%M%S.XX,%d%m%y\r\n", &when);
    (void)snprintf(frac, sizeof(frac), "%.2f", fractional);
    buf[21] = frac[2]; buf[22] = frac[3];
    (void)literal_send(session->gpsdata.gps_fd, buf);

    (void)literal_send(session->gpsdata.gps_fd, "$PFST,START\r\n");
    (void)literal_send(session->gpsdata.gps_fd, "$PFST,SYNCMODE,1\r\n");
    (void)literal_send(session->gpsdata.gps_fd, 
		    ITRAX_MODESTRING, session->gpsdata.baudrate);
}

static bool itrax_speed(struct gps_device_t *session, speed_t speed)
/* change the baud rate */
{
    return literal_send(session->gpsdata.gps_fd, ITRAX_MODESTRING, speed) >= 0;
}

static bool itrax_rate(struct gps_device_t *session, double rate)
/* change the sample rate of the GPS */
{
    return literal_send(session->gpsdata.gps_fd, "$PSFT,FIXRATE,%d\r\n", rate) >= 0;
}

static void itrax_wrap(struct gps_device_t *session)
/* stop navigation, this cuts the power drain */
{
    (void)literal_send(session->gpsdata.gps_fd, "$PFST,SYNCMODE,0\r\n");
    (void)literal_send(session->gpsdata.gps_fd, "$PFST,STOP\r\n");
}

/*@ -redef @*/
static struct gps_type_t itrax = {
    .typename      = "iTrax",		/* full name of type */
    .trigger       = "$PFST,OK",	/* tells us to switch to Itrax */
    .channels      = 12,		/* consumer-grade GPS */
    .probe         = NULL,		/* no probe */
    .initializer   = itrax_initializer,	/* initialize */
    .get_packet    = packet_get,	/* how to get a packet */
    .parse_packet  = nmea_parse_input,	/* how to interpret a packet */
    .rtcm_writer   = NULL,		/* iTrax doesn't support DGPS/WAAS/EGNOS */
    .speed_switcher= itrax_speed,	/* no speed switcher */
    .mode_switcher = NULL,		/* no mode switcher */
    .rate_switcher = itrax_rate,	/* there's a sample-rate switcher */
    .cycle_chars   = 438,		/* not relevant, no rate switch */
    .wrapup         = itrax_wrap,	/* sleep the receiver */
    .cycle          = 1,		/* updates every second */
};
/*@ -redef @*/
#endif /* ITRAX_ENABLE */
#endif /* NMEA_ENABLE */

#ifdef RTCM104_ENABLE
/**************************************************************************
 *
 * RTCM-104, used for broadcasting DGPS corrections and by DGPS radios
 *
 **************************************************************************/

static gps_mask_t rtcm104_analyze(struct gps_device_t *session)
{
    gpsd_report(5, "RTCM packet type 0x%02x length %d words: %s\n", 
		session->gpsdata.rtcm.type,
		session->gpsdata.rtcm.length+2,
		gpsd_hexdump(session->driver.isgps.buf, (session->gpsdata.rtcm.length+2)*sizeof(isgps30bits_t)));
    return RTCM_SET;
}

static struct gps_type_t rtcm104 = {
    .typename      = "RTCM104",		/* full name of type */
    .trigger       = NULL,		/* no recognition string */
    .channels      = 12,		/* consumer-grade GPS */
    .probe         = NULL,		/* no probe */
    .initializer   = NULL,		/* no initializer */
    .get_packet    = packet_get,	/* how to get a packet */
    .parse_packet  = rtcm104_analyze,	/* packet getter does the parsing */
    .rtcm_writer   = NULL,		/* don't send RTCM data,  */
    .speed_switcher= NULL,		/* no speed switcher */
    .mode_switcher = NULL,		/* no mode switcher */
    .rate_switcher = NULL,		/* no sample-rate switcher */
    .cycle_chars   = -1,		/* not relevant, no rate switch */
    .wrapup         = NULL,		/* no wrapup code */
    .cycle          = 1,		/* updates every second */
};
#endif /* RTCM104_ENABLE */

extern struct gps_type_t garmin_binary, sirf_binary, tsip_binary;
extern struct gps_type_t evermore_binary, italk_binary, trueNorth;

/*@ -nullassign @*/
/* the point of this rigamarole is to not have to export a table size */
static struct gps_type_t *gpsd_driver_array[] = {
#ifdef NMEA_ENABLE
    &nmea, 
    &sirfII_nmea,
#if FV18_ENABLE
    &fv18,
    &garmin,
#endif /* FV18_ENABLE */
#if TRIPMATE_ENABLE
    &tripmate,
#endif /* TRIPMATE_ENABLE */
#if EARTHMATE_ENABLE
    &earthmate, 
#endif /* EARTHMATE_ENABLE */
#if ITRAX_ENABLE
    &itrax, 
#endif /* ITRAX_ENABLE */
#endif /* NMEA_ENABLE */
#ifdef ZODIAC_ENABLE
    &zodiac_binary,
#endif /* ZODIAC_ENABLE */
#if GARMIN_ENABLE
    &garmin_binary,
#endif /* GARMIN_ENABLE */
#ifdef SIRFII_ENABLE
    &sirf_binary, 
#endif /* SIRFII_ENABLE */
#ifdef TSIP_ENABLE
    &tsip_binary, 
#endif /* TSIP_ENABLE */
#ifdef TNT_ENABLE
    &trueNorth,
#endif /* TSIP_ENABLE */
#ifdef EVERMORE_ENABLE
    &evermore_binary, 
#endif /* EVERMORE_ENABLE */
#ifdef ITALK_ENABLE
    &italk_binary, 
#endif /* ITALK_ENABLE */
#ifdef RTCM104_ENABLE
    &rtcm104, 
#endif /* RTCM104_ENABLE */
    NULL,
};
/*@ +nullassign @*/
struct gps_type_t **gpsd_drivers = &gpsd_driver_array[0];
