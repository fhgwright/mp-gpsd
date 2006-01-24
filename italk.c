/*
 * Driver for the iTalk binary protocol used by FasTrax
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
#if defined(ITALK_ENABLE) && defined(BINARY_ENABLE)

#include "bits.h"

/*@ +charint -usedef -compdef @*/
static bool italk_write(int fd, unsigned char *msg, size_t msglen) {
   bool      ok;

   /* CONSTRUCT THE MESSAGE */

   /* we may need to dump the message */
   gpsd_report(4, "writing italk control type %02x:%s\n", 
	       msg[0], gpsd_hexdump(msg, msglen));
   ok = (write(fd, msg, msglen) == (ssize_t)msglen);
   (void)tcdrain(fd);
   return(ok);
}
/*@ -charint +usedef +compdef @*/

/*@ +charint @*/
static gps_mask_t italk_parse(struct gps_device_t *session, unsigned char *buf, size_t len)
{
    if (len == 0)
	return 0;

    /* we may need to dump the raw packet */
    gpsd_report(5, "raw italk packet type 0x%02x length %d: %s\n", buf[0], len, gpsd_hexdump(buf, len));

    (void)snprintf(session->gpsdata.tag, sizeof(session->gpsdata.tag),
		   "ITALK%d",(int)buf[0]);

    switch (getub(buf, 0))
    {
	/* DISPATCH ON FIRST BYTE OF PAYLOAD */

    default:
	gpsd_report(3, "unknown iTalk packet id %d length %d: %s\n", buf[0], len, gpsd_hexdump(buf, len));
	return 0;
    }
}
/*@ -charint @*/

static gps_mask_t italk_parse_input(struct gps_device_t *session)
{
    gps_mask_t st;

    if (session->packet_type == ITALK_PACKET){
	st = italk_parse(session, session->outbuffer, session->outbuflen);
	session->gpsdata.driver_mode = 1;
	return st;
#ifdef NMEA_ENABLE
    } else if (session->packet_type == NMEA_PACKET) {
	st = nmea_parse((char *)session->outbuffer, session);
	session->gpsdata.driver_mode = 0;
	return st;
#endif /* NMEA_ENABLE */
    } else
	return 0;
}

static bool italk_set_mode(struct gps_device_t *session UNUSED, 
			      speed_t speed UNUSED, bool mode UNUSED)
{
    /*@ +charint @*/
    unsigned char msg[] = {0,};

    /* HACK THE MESSAGE */

    return italk_write(session->gpsdata.gps_fd, msg, sizeof(msg));
    /*@ +charint @*/
}

static bool italk_speed(struct gps_device_t *session, speed_t speed)
{
    return italk_set_mode(session, speed, true);
}

static void italk_mode(struct gps_device_t *session, int mode)
{
    if (mode == 0) {
	(void)gpsd_switch_driver(session, "Generic NMEA");
	(void)italk_set_mode(session, session->gpsdata.baudrate, false);
	session->gpsdata.driver_mode = 0;
    }
}

static void italk_initializer(struct gps_device_t *session)
/* poll for software version in order to check for old firmware */
{
    if (session->packet_type == NMEA_PACKET)
	(void)italk_set_mode(session, session->gpsdata.baudrate, true);
}

/* this is everything we export */
struct gps_type_t italk_binary =
{
    .typename       = "iTalk binary",	/* full name of type */
    .trigger        = NULL,		/* recognize the type */
    .channels       = 12,		/* consumer-grade GPS */
    .probe          = NULL,		/* no probe */
    .initializer    = italk_initializer,/* initialize the device */
    .get_packet     = packet_get,	/* use generic packet grabber */
    .parse_packet   = italk_parse_input,/* parse message packets */
    .rtcm_writer    = pass_rtcm,	/* send RTCM data straight */
    .speed_switcher = italk_speed,	/* we can change baud rates */
    .mode_switcher  = italk_mode,	/* there is a mode switcher */
    .rate_switcher  = NULL,		/* no sample-rate switcher */
    .cycle_chars    = -1,		/* not relevant, no rate switch */
    .wrapup         = NULL,		/* no close hook */
    .cycle          = 1,		/* updates every second */
};
#endif /* defined(ITALK_ENABLE) && defined(BINARY_ENABLE) */
