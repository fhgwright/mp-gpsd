/*
 * This is the gpsd driver for EverMore GPSes operating in binary mode.
 * About the only thing this gives us that NMEA won't is TDOP.
 * But we'll get atomic position reports from it, which is good.
 *
 * The vendor site is <http://www.emt.com.tw>.
 *
 * This driver was written by Petr Slansky based on a framework by Eric S. 
 * Raymond.  The following remarks are by Petr Slansky.
 *
 * Snooping on the serial the communication between a Windows program and
 * an Evermore chipset reveals some messages not described in the vendor
 * documentation (Issue C of Aug 2002):
 *
 * 10 02 06 84 00 00 00 84 10 03	switch to binary mode (84 00 00 00)
 * 10 02 06 84 01 00 00 85 10 03	switch to NMEA mode (84 01 00 00)
 *
 * 10 02 06 89 01 00 00 8a 10 03        set baud rate 4800
 * 10 02 06 89 01 01 00 8b 10 03        set baud rate 9600
 * 10 02 06 89 01 02 00 8c 10 03        set baud rate 19200
 * 10 02 06 89 01 03 00 8d 10 03        set baud rate 38400
 *
 * 10 02 06 8D 00 01 00 8E 10 03        switch to datum ID 001 (WGS-84)
 * 10 02 06 8D 00 D8 00 65 10 03        switch to datum ID 217 (WGS-72)
 *
 * These don't entail a reset of GPS as the 0x80 message does.
 * 
 * 10 02 04 38 85 bd 10 03     answer from GPS to 0x85 message; Like OK?
 * 10 02 04 38 8d c5 10 03     answer from GPS to 0x8d message; Like OK?
 * 10 02 04 38 8e c6 10 03     answer from GPS to 0x8e message; Like OK?
 * 10 02 04 38 8f c7 10 03     answer from GPS to 0x8f message; Like OK?
 *
 * Message described as 0x89 in the manual is message 0x8f in the
 * actual command set (manual error?).  Message 0x89 is used to switch
 * baud rate without reset.
 * 
 * The chip sometimes sends vendor extension messages with the prefix
 * $PEMT,100. After reset, it sends a $PEMT,100 message describing the
 * chip's configuration.  Here is a sample:
 *
 * $PEMT,100,05.42g,100303,180,05,1,20,15,08,0,0,2,1*5A
 * 100 - message type
 * 05.42g - firmware version
 * 100303 - date of firmware release
 * 180 -  datum ID; 001 is WGS-84
 * 05 - elevation mask; see message 0x86
 * 1 - DOP mode (1 is auto DOP mask); see message 0x87
 * 20 - GDOP; see message 0x87
 * 15 - PDOP
 * 08 - HDOP
 * 0 - Normal mode, without 1PPS
 * 0 - ??
 * 2 - ??
 * 1 - ??
 * 
 * Message $PEMT,100 could be forced with message 0x85:
 * 10 02 12 85 00 00 00 00 00 01 01 00 00 00 00 00 00 00 00 87 10 03
 * This is some kind of initialisation but I don't understand the 0x85
 * message in detail.
 * 
 * With message 0x8e it is possible to define how often each NMEA
 * message is sent (0-255 seconds). It is possible with message 0x8e
 * to activate PEMT,101 messages that have information about time,
 * position, velocity and HDOP.
 * 
 * $PEMT,101,1,03,21.0,230705190757,5004.6612,N,01425.1359,E,00269,045,0000*24
 * 101 - message type
 * 1 - (1 or 2) I guess that this is flag that position is valid
 * 03 - number of used satelites
 * 21.0 - HDOP
 * 230705190757 - date and time, UTC
 * 5004.6612,N - Latitude (degree)
 * 01425.1359,E - Longitude (degree)
 * 00269 - Altitude (m)
 * 045 - heading (degrees from true north)
 * 0000 - ?? could be speed over ground
 * 
 * This is an exampe of an 0x8e message that activates all NMEA sentences 
 * with 1s period:
 * 10 02 12 8E 7F 01 01 01 01 01 01 01 01 00 00 00 00 00 00 15 10 03
 * 
 *
 * There is a way to probe for EverMore chipset. When binary message 0x81 is sent:
 * 10 02 04 81 13 94 10 03
 *
 * EverMore will reply with message like this:
 * *10 *02 *0D *20 E1 00 00 *00 0A 00 1E 00 32 00 5B *10 *03
 * bytes marked with * are fixed
 * Message in reply is information about logging configuration of GPS
 *
 * Other way to probe for EverMore chipset is to send one of messages 
 * 0x85, 0x8d, 0x8e or 0x8f and check for reply.
 * Reply message from EverMore GPS looks like this:
 * *10 *02 *04 *38 8d c5 *10 *03
 * 8d indicates that message 0x8d was sent;
 * c5 is EverMore checksum
 * other bytes are fixed
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <stdio.h>

#include "gpsd.h"
#if defined(EVERMORE_ENABLE) && defined(BINARY_ENABLE)

#define LITTLE_ENDIAN_PROTOCOL
#define GET_ORIGIN 1
#define PUT_ORIGIN 0
#include "bits.h"

#define EVERMORE_CHANNELS	12

/*@ +charint -usedef -compdef @*/
static bool evermore_write(struct gps_device_t *session, unsigned char *msg, size_t msglen)
{
   unsigned int       crc;
   size_t    i, len;
   unsigned char stuffed[MAX_PACKET_LENGTH*2], *cp;
   bool      ok;

   /* prepare a DLE-stuffed copy of the message */
   cp = stuffed;
   *cp++ = 0x10;  /* message starts with DLE STX */
   *cp++ = 0x02;

   len = (size_t)(msglen + 2);   /* msglen < 254 !! */
   *cp++ = (unsigned char)len;   /* message length */
   if (len == 0x10) *cp++ = 0x10;
   
   /* payload */
   crc = 0;
   for (i = 0; i < msglen; i++) {
       *cp++ = msg[i];
       crc += msg[i];
       if (msg[i] == 0x10) *cp++ = 0x10;
   }

   crc &= 0xff;

   /* enter CRC after payload */
   *cp++ = crc;  
   if (crc == 0x10) *cp++ = 0x10;

   *cp++ = 0x10;   /* message ends with DLE ETX */
   *cp++ = 0x03;

   len = (size_t)(cp - stuffed);

   /* we may need to dump the message */
   gpsd_report(4, "writing EverMore control type 0x%02x: %s\n", msg[0], 
	       gpsd_hexdump(stuffed, len));
   ok = gpsd_write(session, stuffed, len);
   return ok;
}
/*@ -charint +usedef +compdef @*/

/*@ +charint @*/
gps_mask_t evermore_parse(struct gps_device_t *session, unsigned char *buf, size_t len)
{
    unsigned char buf2[MAX_PACKET_LENGTH], *cp, *tp;
    size_t i, datalen;
    unsigned int used, visible, satcnt;
    double version;
    gps_mask_t mask = 0;

    if (len == 0)
	return 0;

    /* time to unstuff it and discard the header and footer */
    cp = buf + 2;
    tp = buf2;
    if (*cp == 0x10) cp++;
    datalen = (size_t)*cp++;
   
    gpsd_report(7, "raw EverMore packet type 0x%02x, length %d: %s\n", *cp, len, gpsd_hexdump(buf, len));

    datalen -= 2;

    for (i = 0; i < (size_t)datalen; i++) {
	*tp = *cp++;
	if (*tp == 0x10) cp++;
	tp++;
    }

    /*@ -usedef -compdef @*/
    gpsd_report(6, "EverMore packet type 0x%02x, length %d: %s\n", buf2[0], datalen, gpsd_hexdump(buf2, datalen));
    /*@ +usedef +compdef @*/

    (void)snprintf(session->gpsdata.tag, sizeof(session->gpsdata.tag),
		   "EID%d",(int)buf2[0]);

    switch (getub(buf2, 1))
    {
    case 0x02:	/* Navigation Data Output */
	session->gpsdata.newdata.time = session->gpsdata.sentence_time
	    = gpstime_to_unix((int)getuw(buf2, 2), getul(buf2, 4)*0.01) - session->context->leap_seconds;
	ecef_to_wgs84fix(&session->gpsdata, 
			 getsl(buf2, 8)*1.0, getsl(buf2, 12)*1.0, getsl(buf2, 16)*1.0,
			 getsw(buf2, 20)/10.0, getsw(buf2, 22)/10.0, getsw(buf2, 24)/10.0);
	used = getub(buf2, 26) & 0x0f;
	visible = (getub(buf2, 26) & 0xf0) >> 4;
	version = getuw(buf2, 27)/100.0;
	/* that's all the information in this packet */
	if (used < 3)
	    session->gpsdata.newdata.mode = MODE_NO_FIX;
	else if (used == 3)
	    session->gpsdata.newdata.mode = MODE_2D;
	else {
	    session->gpsdata.newdata.mode = MODE_3D;
	    mask |= ALTITUDE_SET | CLIMB_SET;
	}
	gpsd_report(4, "NDO 0x02: version %3.2f, mode=%d, status=%d, visible=%d, used=%d\n",
		    version,
		    session->gpsdata.newdata.mode,
		    session->gpsdata.status,
		    visible,
		    used);
	mask |= TIME_SET | LATLON_SET | TRACK_SET | SPEED_SET | MODE_SET;
	return mask;

    case 0x04:	/* DOP Data Output */
	session->gpsdata.newdata.time = session->gpsdata.sentence_time
	    = gpstime_to_unix((int)getuw(buf2, 2), getul(buf2, 4)*0.01) - session->context->leap_seconds;
	session->gpsdata.gdop = (double)getub(buf2, 8)*0.1;
	session->gpsdata.pdop = (double)getub(buf2, 9)*0.1;
	session->gpsdata.hdop = (double)getub(buf2, 10)*0.1;
	session->gpsdata.vdop = (double)getub(buf2, 11)*0.1;
	session->gpsdata.tdop = (double)getub(buf2, 12)*0.1;
	switch (getub(buf2, 13)) {
	case 0:	/* no position fix */
	case 1:	/* manual calls this "1D navigation" */
	    session->gpsdata.status = STATUS_NO_FIX;
	    session->gpsdata.newdata.mode = MODE_NO_FIX;
	    break;
	case 2:	/* 2D navigation */
	    session->gpsdata.status = STATUS_FIX;
	    session->gpsdata.newdata.mode = MODE_2D;
	    break;
	case 3:	/* 3D navigation */
	    session->gpsdata.status = STATUS_FIX;
	    session->gpsdata.newdata.mode = MODE_3D;
	    break;
	case 4:	/* 3D navigation with DGPS */
	    session->gpsdata.status = STATUS_DGPS_FIX;
	    session->gpsdata.newdata.mode = MODE_3D;
	    break;
	}
	/* that's all the information in this packet */
	gpsd_report(4, "DDO 0x04: mode=%d, status=%d\n", 
		    session->gpsdata.newdata.mode,
		    session->gpsdata.status);
	return TIME_SET | DOP_SET | MODE_SET | STATUS_SET;

    case 0x06:	/* Channel Status Output */
	session->gpsdata.newdata.time = session->gpsdata.sentence_time
	    = gpstime_to_unix((int)getuw(buf2, 2), getul(buf2, 4)*0.01) - session->context->leap_seconds;
	session->gpsdata.satellites = (int)getub(buf2, 8);
	session->gpsdata.satellites_used = 0;
	memset(session->gpsdata.used, 0, sizeof(session->gpsdata.used));
	if (session->gpsdata.satellites > 12) {
		gpsd_report(4, "Warning: EverMore packet has information about %d satellites!\n",
				session->gpsdata.satellites);
	}
	if (session->gpsdata.satellites > EVERMORE_CHANNELS) session->gpsdata.satellites = EVERMORE_CHANNELS;
	satcnt = 0;
	for (i = 0; i < (size_t)session->gpsdata.satellites; i++) {
	    int prn;
	    // channel = getub(buf2, 7*i+7+2)
            prn = (int)getub(buf2, 7*i+7+3);
	    if (prn == 0) continue;  /* satellite record is not valid */
	    session->gpsdata.PRN[satcnt] = prn;
	    session->gpsdata.azimuth[satcnt] = (int)getuw(buf2, 7*i+7+4);
	    session->gpsdata.elevation[satcnt] = (int)getub(buf2, 7*i+7+6);
	    session->gpsdata.ss[satcnt] = (int)getub(buf2, 7*i+7+7);
	    /*
	     * Status bits at offset 8:
	     * bit0 = 1 satellite acquired
	     * bit1 = 1 code-tracking loop locked
	     * bit2 = 1 carrier-tracking loop locked
	     * bit3 = 1 data-bit synchronization done
	     * bit4 = 1 frame synchronization done
	     * bit5 = 1 ephemeris data collected
	     * bit6 = 1 used for position fix
	     */
	    if (getub(buf2, 7*i+7+8) & 0x40) {
		session->gpsdata.used[session->gpsdata.satellites_used++]=prn;
	    }

	    satcnt++;
		
	}
	session->gpsdata.satellites = (int)satcnt;
	/* that's all the information in this packet */
	gpsd_report(4, "CSO 0x04: %d satellites used\n", 
		    session->gpsdata.satellites_used);
	return TIME_SET | SATELLITE_SET | USED_SET;

    case 0x08:	/* Measurement Data Output */
	// clock offset is a manufacturer diagnostic
	// (int)getuw(buf2, 8);  clock offset, 29000..29850 ??
	session->gpsdata.newdata.time = session->gpsdata.sentence_time
	    = gpstime_to_unix((int)getuw(buf2, 2), getul(buf2, 4)*0.01) - session->context->leap_seconds;
	visible = getub(buf2, 10);
	/* FIXME: read full statellite status for each channel */
	/* gpsd_report(4, "MDO 0x04: visible=%d\n", visible); */
	gpsd_report(4, "MDO 0x04:\n");
	return TIME_SET;
    
    case 0x20:	/* LogConfig Info, could be used as a probe for EverMore GPS */
	gpsd_report(3, "LogConfig EverMore packet, length %d: %s\n", datalen, gpsd_hexdump(buf2, datalen));
	return ONLINE_SET;

    case 0x22:	/* LogData */
	gpsd_report(3, "LogData EverMore packet, length %d: %s\n", datalen, gpsd_hexdump(buf2, datalen));
	return ONLINE_SET;

    default:
	gpsd_report(3, "unknown EverMore packet id 0x%02x, length %d: %s\n", buf2[0], datalen, gpsd_hexdump(buf2, datalen));
	return 0;
    }
}
/*@ -charint @*/

static gps_mask_t evermore_parse_input(struct gps_device_t *session)
{
    gps_mask_t st;

    if (session->packet_type == EVERMORE_PACKET){
	st = evermore_parse(session, session->outbuffer, session->outbuflen);
	session->gpsdata.driver_mode = 1;  /* binary */
	return st;
#ifdef NMEA_ENABLE
    } else if (session->packet_type == NMEA_PACKET) {
	st = nmea_parse((char *)session->outbuffer, session);
	session->gpsdata.driver_mode = 0;  /* NMEA */
	return st;
#endif /* NMEA_ENABLE */
    } else
	return 0;
}

/* configure various EverMore settings to default */
static bool evermore_default(struct gps_device_t *session, int mode)
{
    bool ok = true;
    /*@ +charint @*/
    unsigned char msg86[] = {
	    0x86,	/*  0: msg ID */
	    5,          /*  1: elevation Mask, degree 0..89 */
    };

    unsigned char msg87[] = {
	    0x87,       /*  0: msg ID */
	    1,          /*  1: DOP mask, GDOP(0), auto(1), PDOP(2), HDOP(3), no mask(4) */
	    20,         /*  2: GDOP, 1..99 */
	    15,         /*  3: PDOP, 1..99 */
	    8,          /*  4: HDOP, 1..99 */
    };

#ifdef __UNUSED__
    unsigned char msg8f[] = {
	    0x8f,	/*  0: msg ID */
	    0,          /*  1: operation mode, normal(0), power save(1), 1PPS(2) */
	    1,          /*  2: navigation update rate, 1/Hz, 1..10 */
	    0,          /*  3: RF/GPSBBP On time, 160ms(0), 220(1), 280(2), 340(3), 440(4) */
    };

    unsigned char msg8e[] = {
	    0x8e,	/*  0: msg ID */
	    0x1d,       /*  1: NMEA sentence bitmask, GGA(0), GLL(1), GSA(2), GSV(3), ... */
	    0x01,       /*  2: nmea checksum no(0), yes(1) */
	    1,          /*  3: GPGGA, interval 0-255s */
	    0,          /*  4: GPGLL, interval 0-255s */
	    1,          /*  5: GPGSA, interval 0-255s */
	    1,          /*  6: GPGSV, interval 0-255s */
	    1,          /*  7: GPRMC, interval 0-255s */
	    0,          /*  8: GPVTG, interval 0-255s */
	    0,          /*  9: PEMT,101, interval 0-255s */
	    0, 0, 0, 0, 0, 0, /* 10: ?? */
    };

    unsigned char msg8d[] = {
	    0x8d,       /*  0: msg ID */
	    0x00,       /*  1: 0 (hi byte of datum ID) */
	    0x01,       /*  2: datum ID, 1 is WGS-84 */
	    0x00,       /*  3: ?? */
    };
#endif /* __UNUSED__ */

    unsigned char msg84[] = {
	    0x84,    /* 0: msg ID */
	    0x01,    /* 1: mode; binary(0), nmea(1) */
	    0x00,    /* 2: ?? */
	    0x00,    /* 3: ?? */
    };

    gpsd_report(5, "evermore_default call(%d)\n", mode);
    ok &= evermore_write(session, msg86, sizeof(msg86));
    ok &= evermore_write(session, msg87, sizeof(msg87));
#ifdef __UNUSED__
    ok &= evermore_write(session, msg8f, sizeof(msg8f));
    ok &= evermore_write(session, msg8e, sizeof(msg8e));
    ok &= evermore_write(session, msg8d, sizeof(msg8d));
#endif /* __UNUSED__ */
    if (mode == 1) {
       gpsd_report(1, "Switching chip mode to EverMore binary.\n");
       msg84[1] = 0;  /* binary mode */
    }
    ok &= evermore_write(session, msg84, sizeof(msg84));
    /*@ +charint @*/
    return ok;
}

static bool evermore_set_mode(struct gps_device_t *session,
			      speed_t speed, bool mode)
{
    u_int8_t tmp8;
    double tow;
    int week;

    /*@ +charint @*/
    unsigned char msg[] = {
	    0x80,		        /*  0: msg ID */
	    0x33, 0x05,	                /*  1: GPS week; when 0 is here, we finish with year 1985 */
	    0x00, 0x00, 0x00, 0x00,	/*  3: GPS TOW */
	    0x00, 0x00,			/*  7: Latitude */
	    0x00, 0x00,			/*  9: Longitude */
	    0x00, 0x00,			/* 11: Altitude */
	    0x00, 0x00,			/* 13: Datum ID WGS84 */
	    0x01,			/* 15: hot start */
	    0x5d,			/* 16: bitmask; cksum(6), bin(7), GGA(0), GSA(2), GSV(3), RMC(4) */
	    0,				/* 17: baud rate */
    };
    switch (speed) {
    case 4800:  tmp8 = 0; break;
    case 9600:  tmp8 = 1; break;
    case 19200: tmp8 = 2; break;
    case 38400: tmp8 = 3; break;
    default: return false;
    }
    msg[17] = tmp8;
    session->gpsdata.baudrate = (unsigned int)speed;
    //gpsd_set_speed(session, speed, 'N', 1);
    if (mode) {
        gpsd_report(1, "Switching chip mode to EverMore binary.\n");
	msg[16] |= 0x80;  /* binary mode */
    }
    unix_to_gpstime(timestamp(), &week, &tow);
    putword(msg, 1, (unsigned int) week);
    putlong(msg, 3, (unsigned long) tow);

    return evermore_write(session, msg, sizeof(msg));
    /*@ +charint @*/
}


static bool evermore_speed(struct gps_device_t *session, speed_t speed)
{
#if 0
    gpsd_report(5, "evermore_speed call (%d)\n", speed);
    return evermore_set_mode(session, speed, true);
#else
    unsigned char tmp8;
    unsigned char msg[] = {
	    0x89,          /*  0: msg ID */
	    0x01,          /*  1: ??, maybe mode: normal mode(0), power save(1), 1PPS(2) */
	    0x00,          /*  2: baud rate; 4800(0), 9600(1), 19200(2), 38400(3) */
	    0x00,          /*  3: ??, maybe RF/GPSBB on time; valid only for power save mode */
    };
    gpsd_report(5, "evermore_speed call (%d)\n", speed);
    switch (speed) {
	    case 4800:  tmp8 = 0; break;
	    case 9600:  tmp8 = 1; break;
	    case 19200: tmp8 = 2; break;
	    case 38400: tmp8 = 3; break;
	    default: return false;
    }
    msg[2] = tmp8;
    return evermore_write(session, msg, sizeof(msg));

#endif
}

static void evermore_mode(struct gps_device_t *session, int mode)
{
    gpsd_report(5, "evermore_mode call (%d)\n", mode);
    if (mode == 0) {
	(void)gpsd_switch_driver(session, "Generic NMEA");
	// (void)evermore_set_mode(session, session->gpsdata.baudrate, false);
	(void)evermore_default(session, 0);
	session->gpsdata.driver_mode = 0;
    } else {
	(void)evermore_default(session, 1);
	session->gpsdata.driver_mode = 1;
    }
}

static void evermore_initializer(struct gps_device_t *session)
/* poll for software version in order to check for old firmware */
{
    gpsd_report(5, "evermore_initializer call\n");
    if (session->packet_type == NMEA_PACKET)
	(void)evermore_set_mode(session, session->gpsdata.baudrate, true);
    (void)evermore_default(session, 0);
}

#ifdef __UNUSED__
static void evermore_probe(struct gps_device_t *session)
/* send a binary message to probe for EverMore GPS */
/*
 * There is a way to probe for EverMore chipset. When binary message 0x81 is sent:
 * 10 02 04 81 13 94 10 03
 *
 * EverMore will reply with something like this:
 * *10 *02 *0D *20 E1 00 00 *00 0A 00 1E 00 32 00 5B *10 *03
 * bytes marked with * are fixed
 * Message in reply is information about logging configuration of GPS
 * */
{
   unsigned char msg[] = {
	   0x81, 	/*  0: msg ID */
	   0x13,        /*  1: LogRead = 0x13 */
   };
   bool ok;
   gpsd_report(5, "evermore_probe call\n");
   ok = evermore_write(session, msg, sizeof(msg));
   return;
}
#endif /* __UNUSED__ */

#ifdef __UNUSED__
static void evermore_close(struct gps_device_t *session)
/* set GPS to NMEA, 4800, GGA, GSA, GSV, RMC (default) */ 
{	
	gpsd_report(5, "evermore_close call\n");
	(void)evermore_set_mode(session, 4800, false);
}
#endif /* __UNUSED__ */

/* this is everything we export */
struct gps_type_t evermore_binary =
{
    .typename       = "EverMore binary",	/* full name of type */
    //.trigger        = "$PEMT,100,05.",	recognize the type */
    .trigger        = "\x10\x02\x04\x38\x8d\xc5\x10\x03",
    .channels       = EVERMORE_CHANNELS,	/* consumer-grade GPS */
    .probe          = NULL,			/* no probe */
    .initializer    = evermore_initializer,	/* initialize the device */
    .get_packet     = packet_get,		/* use generic one */
    .parse_packet   = evermore_parse_input,	/* parse message packets */
    .rtcm_writer    = pass_rtcm,		/* send RTCM data straight */
    .speed_switcher = evermore_speed,		/* we can change baud rates */
    .mode_switcher  = evermore_mode,		/* there is a mode switcher */
    .rate_switcher  = NULL,			/* no sample-rate switcher */
    .cycle_chars    = -1,			/* ignore, no rate switch */
    .wrapup         = NULL,			/* ignore, no wrapup */
    .cycle          = 1,			/* updates every second */
};
#endif /* defined(EVERMORE_ENABLE) && defined(BINARY_ENABLE) */
