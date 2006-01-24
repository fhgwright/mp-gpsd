/*
 * Handle the Trimble TSIP packet format
 * by Rob Janssen, PE1CHL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "gpsd.h"
#include "bits.h"

#define USE_SUPERPACKET	1			/* use Super Packet mode? */

#define SEMI_2_DEG	(180.0 / 2147483647)	/* 2^-31 semicircle to deg */

#ifdef TSIP_ENABLE
#define TSIP_CHANNELS	12

static int tsip_write(int fd, unsigned int id, unsigned char *buf, size_t len)
{
    char buf2[BUFSIZ];

    gpsd_report(5, "Sent TSIP packet id 0x%02x: %s\n",id,gpsd_hexdump(buf,len));

    /*@ +charint @*/
    buf2[0] = '\x10';
    buf2[1] = (char)id;
    if (write(fd,buf2,2) != 2)
	return -1;

    while (len-- > 0) {
	if (*buf == '\x10')
	    if (write(fd,buf2,1) != 1)
		return -1;

	if (write(fd,buf++,1) != 1)
	    return -1;
    }

    buf2[1] = '\x03';
    /*@ -charint @*/
    if (write(fd,buf2,2) != 2)
	return -1;

    return 0;
}

static void tsip_initializer(struct gps_device_t *session)
{
    unsigned char buf[100];

    /* TSIP is ODD parity 1 stopbit, save original values and change it */
    session->driver.tsip.parity = session->gpsdata.parity;
    session->driver.tsip.stopbits = session->gpsdata.stopbits;
    gpsd_set_speed(session, session->gpsdata.baudrate, 'O', 1);

    /* I/O Options */
    putbyte(buf,0,0x1e);		/* Position: DP, MSL, LLA */
    putbyte(buf,1,0x02);		/* Velocity: ENU */
    putbyte(buf,2,0x00);		/* Time: GPS */
    putbyte(buf,3,0x08);		/* Aux: dBHz */
    (void)tsip_write(session->gpsdata.gps_fd, 0x35, buf, 4);

    /* Request Software Versions */
    (void)tsip_write(session->gpsdata.gps_fd, 0x1f, buf, 0);

    /* Request Current Time */
    (void)tsip_write(session->gpsdata.gps_fd, 0x21, buf, 0);

    /* Request GPS Systems Message */
    (void)tsip_write(session->gpsdata.gps_fd, 0x28, buf, 0);

    /* Request Current Datum Values */
    putbyte(buf,0,0x15);
    (void)tsip_write(session->gpsdata.gps_fd, 0x8e, buf, 1);

    /* Request Navigation Configuration */
    putbyte(buf,0,0x03);
    (void)tsip_write(session->gpsdata.gps_fd, 0xbb, buf, 1);
}

static void tsip_wrapup(struct gps_device_t *session)
{
    /* restore saved parity and stopbits when leaving TSIP mode */
    gpsd_set_speed(session, 
		   session->gpsdata.baudrate,
		   (unsigned char)session->driver.tsip.parity, 
		   session->driver.tsip.stopbits);
}

static bool tsip_speed_switch(struct gps_device_t *session, unsigned int speed)
{
    unsigned char buf[100];

    putbyte(buf,0,0xff);		/* current port */
    putbyte(buf,1,(round(log((double)speed/300)/M_LN2))+2); /* input baudrate */
    putbyte(buf,2,getub(buf,1));	/* output baudrate */
    putbyte(buf,3,8);			/* character width (8 bits) */
    putbyte(buf,4,1);			/* parity (odd) */
    putbyte(buf,5,0);			/* stop bits (1 stopbit) */
    putbyte(buf,6,0);			/* flow control (none) */
    putbyte(buf,7,0x02);		/* input protocol (TSIP) */
    putbyte(buf,8,0x02);		/* output protocol (TSIP) */
    putbyte(buf,9,0);			/* reserved */
    (void)tsip_write(session->gpsdata.gps_fd, 0xbc, buf, 10);

    return true;	/* it would be nice to error-check this */
}

static gps_mask_t tsip_analyze(struct gps_device_t *session)
{
    int i, j, len, count;
    gps_mask_t mask = 0;
    unsigned int id;
    u_int8_t u1,u2,u3,u4,u5;
    int16_t s1,s2,s3,s4;
    int32_t sl1,sl2,sl3;
    u_int32_t ul1,ul2;
    float f1,f2,f3,f4,f5;
    double d1,d2,d3,d4,d5;
    union int_float i_f;
    union long_double l_d;
    time_t now;
    unsigned char buf[BUFSIZ];
    char buf2[BUFSIZ];

    if (session->packet_type != TSIP_PACKET) {
	gpsd_report(2, "tsip_analyze packet type %d\n",session->packet_type);
	return 0;
    }

    /*@ +charint @*/
    if (session->outbuflen < 4 || session->outbuffer[0] != 0x10)
	return 0;

    /* remove DLE stuffing and put data part of message in buf */

    memset(buf, 0, sizeof(buf));
    buf2[len = 0] = '\0';
    for (i = 2; i < (int)session->outbuflen; i++) {
	if (session->outbuffer[i] == 0x10)
	    if (session->outbuffer[++i] == 0x03)
		break;

	(void)snprintf(buf2+strlen(buf2), 
		      sizeof(buf2)-strlen(buf2),
		      "%02x", buf[len++] = session->outbuffer[i]);
    }
    /*@ -charint @*/

    (void)snprintf(session->gpsdata.tag, sizeof(session->gpsdata.tag), 
		   "ID%02x", id = (unsigned)session->outbuffer[1]);

    gpsd_report(5, "TSIP packet id 0x%02x length %d: %s\n",id,len,buf2);
    (void)time(&now);

    switch (id) {
    case 0x13:		/* Packet Received */
	u1 = getub(buf,0);
	u2 = getub(buf,1);
	gpsd_report(4, "Received packet of type %02x cannot be parsed\n",u1);
#if USE_SUPERPACKET
	if ((int)u1 == 0x8e && (int)u2 == 0x23) { /* no Compact Super Packet */
	    gpsd_report(2, "No Compact Super Packet, use LFwEI\n");

	    /* Request LFwEI Super Packet */
	    putbyte(buf,0,0x20);
	    putbyte(buf,1,0x01);		/* enabled */
	    (void)tsip_write(session->gpsdata.gps_fd, 0x8e, buf, 2);
	}
#endif /* USE_SUPERPACKET */
	break;
    case 0x41:		/* GPS Time */
	if (len != 10)
	    break;
	session->driver.tsip.last_41 = now;			/* keep timestamp for request */
	f1 = getf(buf,0);			/* gpstime */
	s1 = getsw(buf,4);			/* week */
	f2 = getf(buf,6);			/* leap seconds */
	if (f1 >= 0.0 && f2 > 10.0) {
	    session->driver.tsip.gps_week = s1;
	    session->context->leap_seconds = (int)roundf(f2);
	    session->context->valid |= LEAP_SECOND_VALID;

	    session->gpsdata.sentence_time = gpstime_to_unix((int)s1, f1) - f2;

#ifdef NTPSHM_ENABLE
	    (void)ntpshm_put(session, session->gpsdata.sentence_time + 0.075);
#endif
	    mask |= TIME_SET;
	}
	gpsd_report(4, "GPS Time %f %d %f\n",f1,s1,f2);
	break;
    case 0x42:		/* Single-Precision Position Fix, XYZ ECEF */
	if (len != 16)
	    break;
	f1 = getf(buf,0);			/* X */
	f2 = getf(buf,4);			/* Y */
	f3 = getf(buf,8);			/* Z */
	f4 = getf(buf,12);			/* time-of-fix */
	gpsd_report(4, "GPS Position XYZ %f %f %f %f\n",f1,f2,f3,f4);
	break;
    case 0x43:		/* Velocity Fix, XYZ ECEF */
	if (len != 20)
	    break;
	f1 = getf(buf,0);			/* X velocity */
	f2 = getf(buf,4);			/* Y velocity */
	f3 = getf(buf,8);			/* Z velocity */
	f4 = getf(buf,12);			/* bias rate */
	f5 = getf(buf,16);			/* time-of-fix */
	gpsd_report(4, "GPS Velocity XYZ %f %f %f %f %f\n",f1,f2,f3,f4,f5);
	break;
    case 0x45:		/* Software Version Information */
	if (len != 10)
	    break;
	gpsd_report(4, "Software versions %d.%d %02d%02d%02d %d.%d %02d%02d%02d\n",
		getub(buf,0),getub(buf,1),getub(buf,4),getub(buf,2),getub(buf,3),
		getub(buf,5),getub(buf,6),getub(buf,9),getub(buf,7),getub(buf,8));
	break;
    case 0x46:		/* Health of Receiver */
	if (len != 2)
	    break;
	u1 = getub(buf,0);			/* Status code */
	u2 = getub(buf,1);			/* Antenna/Battery */
	gpsd_report(4, "Receiver health %02x %02x\n",u1,u2);
	break;
    case 0x47:		/* Signal Levels for all Satellites */
	count = (int)getub(buf,0);		/* satellite count */
	if (len != (5*count + 1))
	    break;
	buf2[0] = '\0';
	for (i = 0; i < count; i++) {
	    u1 = getub(buf,5*i + 1);
	    if ((f1 = getf(buf,5*i + 2)) < 0)
		f1 = 0.0;
	    for (j = 0; j < TSIP_CHANNELS; j++)
		if (session->gpsdata.PRN[j] == (int)u1) {
		    session->gpsdata.ss[j] = (int)roundf(f1);
		    break;
		}
	    (void)snprintf(buf2+strlen(buf2), sizeof(buf2)-strlen(buf2),
			   " %d=%.1f",(int)u1,f1);
	}
	gpsd_report(4, "Signal Levels (%d):%s\n",count,buf2);
	mask |= SATELLITE_SET;
	break;
    case 0x48:		/* GPS System Message */
	buf[len] = '\0';
	gpsd_report(4, "GPS System Message: %s\n",buf);
	break;
    case 0x4a:		/* Single-Precision Position LLA */
	if (len != 20)
	    break;
	session->gpsdata.newdata.latitude  = getf(buf,0) * RAD_2_DEG;
	session->gpsdata.newdata.longitude = getf(buf,4) * RAD_2_DEG;
	session->gpsdata.newdata.altitude  = getf(buf,8);
	f1 = getf(buf,12);			/* clock bias */
	f2 = getf(buf,16);			/* time-of-fix */
	if (session->driver.tsip.gps_week) {
	    session->gpsdata.newdata.time = session->gpsdata.sentence_time =
		gpstime_to_unix((int)session->driver.tsip.gps_week, f2) - session->context->leap_seconds;
	    mask |= TIME_SET;
	}
	gpsd_report(4, "GPS LLA %f %f %f\n",session->gpsdata.newdata.latitude,session->gpsdata.newdata.longitude,session->gpsdata.newdata.altitude);
	mask |= LATLON_SET | ALTITUDE_SET | CYCLE_START_SET;
	break;
    case 0x4b:		/* Machine/Code ID and Additional Status */
	if (len != 3)
	    break;
	u1 = getub(buf,0);			/* Machine ID */
	u2 = getub(buf,1);			/* Status 1 */
	u3 = getub(buf,2);			/* Status 2 */
	gpsd_report(4, "Machine ID %02x %02x %02x\n",u1,u2,u3);
#if USE_SUPERPACKET
	if ((u3 & 0x01) != (u_int8_t)0 && !session->driver.tsip.superpkt) {
	    gpsd_report(2, "Switching to Super Packet mode\n");

	    /* set new I/O Options for Super Packet output */
	    putbyte(buf,0,0x2c);		/* Position: SP, MSL */
	    putbyte(buf,1,0x00);		/* Velocity: none (via SP) */
	    putbyte(buf,2,0x00);		/* Time: GPS */
	    putbyte(buf,3,0x08);		/* Aux: dBHz */
	    (void)tsip_write(session->gpsdata.gps_fd, 0x35, buf, 4);
	    session->driver.tsip.superpkt = true;
	}
#endif /* USE_SUPERPACKET */
	break;
    case 0x55:		/* IO Options */
	if (len != 4)
	    break;
	u1 = getub(buf,0);			/* Position */
	u2 = getub(buf,1);			/* Velocity */
	u3 = getub(buf,2);			/* Timing */
	u4 = getub(buf,3);			/* Aux */
	gpsd_report(4, "IO Options %02x %02x %02x %02x\n",u1,u2,u3,u4);
#if USE_SUPERPACKET
	if ((u1 & 0x20) != (u_int8_t)0) {	/* Output Super Packets? */
	    /* No LFwEI Super Packet */
	    putbyte(buf,0,0x20);
	    putbyte(buf,1,0x00);		/* disabled */
	    (void)tsip_write(session->gpsdata.gps_fd, 0x8e, buf, 2);

	    /* Request Compact Super Packet */
	    putbyte(buf,0,0x23);
	    putbyte(buf,1,0x01);		/* enabled */
	    (void)tsip_write(session->gpsdata.gps_fd, 0x8e, buf, 2);
	}
#endif /* USE_SUPERPACKET */
	break;
    case 0x56:		/* Velocity Fix, East-North-Up (ENU) */
	if (len != 20)
	    break;
	f1 = getf(buf,0);			/* East velocity */
	f2 = getf(buf,4);			/* North velocity */
	f3 = getf(buf,8);			/* Up velocity */
	f4 = getf(buf,12);			/* clock bias rate */
	f5 = getf(buf,16);			/* time-of-fix */
	session->gpsdata.newdata.climb = f3;
	/*@ -evalorder @*/
	session->gpsdata.newdata.speed = sqrt(pow(f2,2) + pow(f1,2));
	/*@ +evalorder @*/
	if ((session->gpsdata.newdata.track = atan2(f1,f2) * RAD_2_DEG) < 0)
	    session->gpsdata.newdata.track += 360.0;
	gpsd_report(4, "GPS Velocity ENU %f %f %f %f %f\n",f1,f2,f3,f4,f5);
	mask |= SPEED_SET | TRACK_SET | CLIMB_SET; 
	break;
    case 0x57:		/* Information About Last Computed Fix */
	if (len != 8)
	    break;
	u1 = getub(buf,0);			/* Source of information */
	u2 = getub(buf,1);			/* Mfg. diagnostic */
	f1 = getf(buf,2);			/* gps_time */
	s1 = getsw(buf,6);			/* tsip.gps_week */
	/*@ +charint @*/
	if (getub(buf,0) == 0x01)		/* good current fix? */
	    session->driver.tsip.gps_week = s1;
	/*@ -charint @*/
	gpsd_report(4, "Fix info %02x %02x %d %f\n",u1,u2,s1,f1);
	break;
    case 0x58:		/* Satellite System Data/Acknowledge from Receiver */
	break;
    case 0x59:		/* Status of Satellite Disable or Ignore Health */
	break;
    case 0x5a:		/* Raw Measurement Data */
	if (len != 25)
	    break;
	f1 = getf(buf,5);			/* Signal Level */
	f2 = getf(buf,9);			/* Code phase */
	f3 = getf(buf,13);			/* Doppler */
	d1 = getd(buf,17);			/* Time of Measurement */
	gpsd_report(4, "Raw Measurement Data %d %f %f %f %f\n",getub(buf,0),f1,f2,f3,d1);
	break;
    case 0x5c:		/* Satellite Tracking Status */
	if (len != 24)
	    break;
	session->driver.tsip.last_5c = now;		/* keep timestamp for request */
	u1 = getub(buf,0);			/* PRN */
	u2 = getub(buf,1);			/* chan */
	u3 = getub(buf,2);			/* Acquisition flag */
	u4 = getub(buf,3);			/* Ephemeris flag */
	f1 = getf(buf,4);			/* Signal level */
	f2 = getf(buf,8);			/* time of Last measurement */
	d1 = getf(buf,12) * RAD_2_DEG;		/* Elevation */
	d2 = getf(buf,16) * RAD_2_DEG;		/* Azimuth */
	i = (int)(u2 >> 3);			/* channel number */
	gpsd_report(4, "Satellite Tracking Status %d: %2d 0x%02x %d %d %.1f %f %.1f %.1f\n",i,u1,u2,u3,u4,f1,f2,d1,d2);
	if (i < TSIP_CHANNELS) {
	    if (d1 >= 0.0) {
		session->gpsdata.PRN[i] = (int)u1;
		session->gpsdata.ss[i] = (int)roundf(f1);
		session->gpsdata.elevation[i] = (int)round(d1);
		session->gpsdata.azimuth[i] = (int)round(d2);
	    } else {
		session->gpsdata.PRN[i] = session->gpsdata.ss[i] =
		session->gpsdata.elevation[i] = session->gpsdata.azimuth[i] = 0;
	    }
	    if (++i == session->gpsdata.satellites)
		mask |= SATELLITE_SET;		/* last of the series */
	    if (i > session->gpsdata.satellites)
		session->gpsdata.satellites = i;
	}
	break;
    case 0x6d:		/* All-In-View Satellite Selection */
	u1 = getub(buf,0);			/* nsvs/dimension */
	count = (int)((u1 >> 4) & 0x0f);
	if (len != (17 + count))
	    break;
	session->driver.tsip.last_6d = now;		/* keep timestamp for request */
	switch (u1 & 7)				/* dimension */
	{
	case 3:
	    session->gpsdata.status = STATUS_FIX;
	    session->gpsdata.newdata.mode = MODE_2D;
	    break;
	case 4:
	    session->gpsdata.status = STATUS_FIX;
	    session->gpsdata.newdata.mode = MODE_3D;
	    break;
	default:
	    session->gpsdata.status = STATUS_NO_FIX;
	    session->gpsdata.newdata.mode = MODE_NO_FIX;
	    break;
	}
	session->gpsdata.satellites_used = count;
	session->gpsdata.pdop = getf(buf,1);
	session->gpsdata.hdop = getf(buf,5);
	session->gpsdata.vdop = getf(buf,9);
	session->gpsdata.tdop = getf(buf,13);
	/*@ -evalorder @*/
	session->gpsdata.gdop = sqrt(pow(session->gpsdata.pdop,2)+pow(session->gpsdata.tdop,2));
	/*@ +evalorder @*/

	memset(session->gpsdata.used,0,sizeof(session->gpsdata.used));
	buf2[0] = '\0';
	/*@ +charint @*/
	for (i = 0; i < count; i++)
	    (void)snprintf(buf2+strlen(buf2), sizeof(buf2)-strlen(buf2),
			" %d",session->gpsdata.used[i] = getub(buf,17 + i));
	/*@ -charint @*/

	gpsd_report(4, "Sat info: %d %f %f %f %f %f %d:%s\n",
		session->gpsdata.newdata.mode, session->gpsdata.pdop,
		session->gpsdata.hdop, session->gpsdata.vdop,
		session->gpsdata.tdop, session->gpsdata.gdop,
		session->gpsdata.satellites_used,buf2);
        mask |= HDOP_SET | VDOP_SET | PDOP_SET | TDOP_SET | GDOP_SET | STATUS_SET | MODE_SET | USED_SET;
	break;
    case 0x6e:		/* Synchronized Measurements */
	break;
    case 0x6f:		/* Synchronized Measurements Report */
	/*@ +charint @*/
	if (len < 20 || getub(buf,0) != 1 || getub(buf,1) != 2)
	    break;
	/*@ -charint @*/
	s1 = getsw(buf,2);			/* number of bytes */
	u1 = getub(buf,20);			/* number of SVs */
	break;
    case 0x70:		/* Filter Report */
	break;
    case 0x7a:		/* NMEA settings */
	break;
    case 0x82:		/* Differential Position Fix Mode */
	if (len != 1)
	    break;
	u1 = getub(buf,0);			/* fix mode */
	/*@ +charint @*/
	if (session->gpsdata.status == STATUS_FIX && (u1 & 0x01)!=0) {
	    session->gpsdata.status = STATUS_DGPS_FIX;
	    mask |= STATUS_SET;
	}
	/*@ -charint @*/
	gpsd_report(4, "DGPS mode %d\n",u1);
	break;
    case 0x83:		/* Double-Precision XYZ Position Fix and Bias Information */
	if (len != 36)
	    break;
	d1 = getd(buf,0);			/* X */
	d2 = getd(buf,8);			/* Y */
	d3 = getd(buf,16);			/* Z */
	d4 = getd(buf,24);			/* clock bias */
	f1 = getf(buf,32);			/* time-of-fix */
	gpsd_report(4, "GPS Position XYZ %f %f %f %f %f\n",d1,d2,d3,d4,f1);
	break;
    case 0x84:		/* Double-Precision LLA Position Fix and Bias Information */
	if (len != 36)
	    break;
	session->gpsdata.newdata.latitude  = getd(buf,0) * RAD_2_DEG;
	session->gpsdata.newdata.longitude = getd(buf,8) * RAD_2_DEG;
	session->gpsdata.newdata.altitude  = getd(buf,16);
	d1 = getd(buf,24);			/* clock bias */
	f1 = getf(buf,32);			/* time-of-fix */
	if (session->driver.tsip.gps_week) {
	    session->gpsdata.newdata.time = session->gpsdata.sentence_time =
		gpstime_to_unix((int)session->driver.tsip.gps_week, f1) - session->context->leap_seconds;
	    mask |= TIME_SET;
	}
	gpsd_report(4, "GPS DP LLA %f %f %f\n",session->gpsdata.newdata.latitude,session->gpsdata.newdata.longitude,session->gpsdata.newdata.altitude);
	mask |= LATLON_SET | ALTITUDE_SET | CYCLE_START_SET;
	break;
    case 0x8f:		/* Super Packet.  Well...  */
	/*@ +charint @*/
	(void)snprintf(session->gpsdata.tag+strlen(session->gpsdata.tag),
		       sizeof(session->gpsdata.tag)-strlen(session->gpsdata.tag), 
		       "%02x", u1 = getub(buf,0));
	/*@ -charint @*/
	switch (u1)				/* sub-packet ID */
	{
	case 0x15:	/* Current Datum Values */
	    if (len != 43)
		break;
	    s1 = getsw(buf,1);			/* Datum Index */
	    d1 = getd(buf,3);			/* DX */
	    d2 = getd(buf,11);			/* DY */
	    d3 = getd(buf,19);			/* DZ */
	    d4 = getd(buf,27);			/* A-axis */
	    d5 = getd(buf,35);			/* Eccentricity Squared */
	    gpsd_report(4, "Current Datum %d %f %f %f %f %f\n",s1,d1,d2,d3,d4,d5);
	    break;

	case 0x20:	/* Last Fix with Extra Information (binary fixed point) */
	    if (len != 56)
		break;
	    s1 = getsw(buf,2);			/* east velocity */
	    s2 = getsw(buf,4);			/* north velocity */
	    s3 = getsw(buf,6);			/* up velocity */
	    ul1 = getul(buf,8);			/* time */
	    sl1 = getsl(buf,12);		/* latitude */
	    ul2 = getul(buf,16);		/* longitude */
	    sl2 = getsl(buf,20);		/* altitude */
	    u1 = getub(buf,24);			/* velocity scaling */
	    u2 = getub(buf,27);			/* fix flags */
	    u3 = getub(buf,28);			/* num svs */
	    u4 = getub(buf,29);			/* utc offset */
	    s4 = getsw(buf,30);			/* tsip.gps_week */
						/* PRN/IODE data follows */
	    gpsd_report(4, "LFwEI %d %d %d %u %d %u %u %x %x %u %u %d\n",s1,s2,s3,ul1,sl1,ul2,sl2,u1,u2,u3,u4,s4);

	    if ((u1 & 0x01) != (u_int8_t)0)	/* check velocity scaling */
		d5 = 0.02;
	    else
		d5 = 0.005;
	    d1 = s1 * d5;			/* east velocity m/s */
	    d2 = s2 * d5;			/* north velocity m/s */
	    session->gpsdata.newdata.climb = s3 * d5; /* up velocity m/s */
	    /*@ -evalorder @*/
	    session->gpsdata.newdata.speed = sqrt(pow(d2,2) + pow(d1,2));
	    /*@ +evalorder @*/
	    if ((session->gpsdata.newdata.track = atan2(d1,d2) * RAD_2_DEG) < 0)
		session->gpsdata.newdata.track += 360.0;
	    session->gpsdata.newdata.latitude  = sl1 * SEMI_2_DEG;
	    if ((session->gpsdata.newdata.longitude = ul2 * SEMI_2_DEG) > 180.0)
		session->gpsdata.newdata.longitude -= 360.0;
	    session->gpsdata.separation = wgs84_separation(session->gpsdata.newdata.latitude, session->gpsdata.newdata.longitude);
	    session->gpsdata.newdata.altitude  = sl2 * 1e-3 - session->gpsdata.separation;;
	    session->gpsdata.status = STATUS_NO_FIX;
	    session->gpsdata.newdata.mode = MODE_NO_FIX;
	    if ((u2 & 0x01) == (u_int8_t)0) {	/* Fix Available */
		session->gpsdata.status = STATUS_FIX;
		if ((u2 & 0x02) != (u_int8_t)0)	/* DGPS Corrected */
		    session->gpsdata.status = STATUS_DGPS_FIX;
		if ((u2 & 0x04) != (u_int8_t)0)	/* Fix Dimension */
		    session->gpsdata.newdata.mode = MODE_2D;
		else
		    session->gpsdata.newdata.mode = MODE_3D;
	    }
	    session->gpsdata.satellites_used = (int)u3;
	    if ((int)u4 > 10) {
		session->context->leap_seconds = (int)u4;
		session->context->valid |= LEAP_SECOND_VALID;
	    }
	    session->driver.tsip.gps_week = s4;
	    session->gpsdata.newdata.time = session->gpsdata.sentence_time =
		gpstime_to_unix((int)s4, ul1 * 1e-3) - session->context->leap_seconds;
	    mask |= TIME_SET | LATLON_SET | ALTITUDE_SET | SPEED_SET | TRACK_SET | CLIMB_SET | STATUS_SET | MODE_SET | CYCLE_START_SET; 
	    break;
	case 0x23:	/* Compact Super Packet */
	    if (len != 29)
		break;
	    ul1 = getul(buf,1);			/* time */
	    s1 = getsw(buf,5);			/* tsip.gps_week */
	    u1 = getub(buf,7);			/* utc offset */
	    u2 = getub(buf,8);			/* fix flags */
	    sl1 = getsl(buf,9);			/* latitude */
	    ul2 = getul(buf,13);		/* longitude */
	    sl3 = getsl(buf,17);		/* altitude */
	    s2 = getsw(buf,21);			/* east velocity */
	    s3 = getsw(buf,23);			/* north velocity */
	    s4 = getsw(buf,25);			/* up velocity */
	    gpsd_report(4, "CSP %u %d %u %u %d %u %d %d %d %d\n",ul1,s1,u1,u2,sl1,ul2,sl3,s2,s3,s4);
	    session->driver.tsip.gps_week = s1;
	    if ((int)u1 > 10) {
		session->context->leap_seconds = (int)u1;
		session->context->valid |= LEAP_SECOND_VALID;
	    }
	    session->gpsdata.newdata.time = session->gpsdata.sentence_time =
		gpstime_to_unix((int)s1, ul1 * 1e-3) - session->context->leap_seconds;
	    session->gpsdata.status = STATUS_NO_FIX;
	    session->gpsdata.newdata.mode = MODE_NO_FIX;
	    if ((u2 & 0x01) == (u_int8_t)0) {	/* Fix Available */
		session->gpsdata.status = STATUS_FIX;
		if ((u2 & 0x02) != (u_int8_t)0)	/* DGPS Corrected */
		    session->gpsdata.status = STATUS_DGPS_FIX;
		if ((u2 & 0x04) != (u_int8_t)0)	/* Fix Dimension */
		    session->gpsdata.newdata.mode = MODE_2D;
		else
		    session->gpsdata.newdata.mode = MODE_3D;
	    }
	    session->gpsdata.newdata.latitude  = sl1 * SEMI_2_DEG;
	    if ((session->gpsdata.newdata.longitude = ul2 * SEMI_2_DEG) > 180.0)
		session->gpsdata.newdata.longitude -= 360.0;
	    session->gpsdata.separation = wgs84_separation(session->gpsdata.newdata.latitude, session->gpsdata.newdata.longitude);
	    session->gpsdata.newdata.altitude  = sl3 * 1e-3 - session->gpsdata.separation;;
	    if ((u2 & 0x20) != (u_int8_t)0)	/* check velocity scaling */
		d5 = 0.02;
	    else
		d5 = 0.005;
	    d1 = s2 * d5;			/* east velocity m/s */
	    d2 = s3 * d5;			/* north velocity m/s */
	    session->gpsdata.newdata.climb = s4 * d5; /* up velocity m/s */
	    /*@ -evalorder @*/
	    session->gpsdata.newdata.speed = sqrt(pow(d2,2) + pow(d1,2));
	    /*@ +evalorder @*/
	    if ((session->gpsdata.newdata.track = atan2(d1,d2) * RAD_2_DEG) < 0)
		session->gpsdata.newdata.track += 360.0;
	    mask |= TIME_SET | LATLON_SET | ALTITUDE_SET | SPEED_SET | TRACK_SET | CLIMB_SET | STATUS_SET | MODE_SET | CYCLE_START_SET; 
	    break;
	default:
	    gpsd_report(4,"Unhandled TSIP superpacket type 0x%02x\n",u1);
	}
	break;
    case 0xbb:		/* Navigation Configuration */
	if (len != 40)
	    break;
	u1 = getub(buf,0);			/* Subcode */
	u2 = getub(buf,1);			/* Operating Dimension */
	u3 = getub(buf,2);			/* DGPS Mode */
	u4 = getub(buf,3);			/* Dynamics Code */
	f1 = getf(buf,5);			/* Elevation Mask */
	f2 = getf(buf,9);			/* AMU Mask */
	f3 = getf(buf,13);			/* DOP Mask */
	f4 = getf(buf,17);			/* DOP Switch */
	u5 = getub(buf,21);			/* DGPS Age Limit */
	gpsd_report(4, "Navigation Configuration %u %u %u %u %f %f %f %f %u\n",u1,u2,u3,u4,f1,f2,f3,f4,u5);
	break;
    default:
	gpsd_report(4,"Unhandled TSIP packet type 0x%02x\n",id);
	break;
    }

    /* see if it is time to send some request packets for reports that */
    /* the receiver won't send at fixed intervals */

    if ((now - session->driver.tsip.last_41) > 5) {
	/* Request Current Time */
	(void)tsip_write(session->gpsdata.gps_fd, 0x21, buf, 0);
	session->driver.tsip.last_41 = now;
    }

    if ((now - session->driver.tsip.last_6d) > 5) {
	/* Request GPS Receiver Position Fix Mode */
	(void)tsip_write(session->gpsdata.gps_fd, 0x24, buf, 0);
	session->driver.tsip.last_6d = now;
    }

    if ((now - session->driver.tsip.last_5c) >= 5) {
	/* Request Current Satellite Tracking Status */
	putbyte(buf,0,0x00);		/* All satellites */
	(void)tsip_write(session->gpsdata.gps_fd, 0x3c, buf, 1);
	session->driver.tsip.last_5c = now;
    }

    return mask;
}

static gps_mask_t tsip_parse_input(struct gps_device_t *session)
{
    gps_mask_t st;

    if (session->packet_type == TSIP_PACKET){
	st = tsip_analyze(session);
	session->gpsdata.driver_mode = 1;
	return st;
#ifdef EVERMORE_ENABLE
    } else if (session->packet_type == EVERMORE_PACKET) {
	(void)gpsd_switch_driver(session, "EverMore binary");
	st = evermore_parse(session, session->outbuffer, session->outbuflen);
	session->gpsdata.driver_mode = 0;
	return st;
#endif /* EVERMORE_ENABLE */
    } else
	return 0;
}

/* this is everything we export */
struct gps_type_t tsip_binary =
{
    .typename       = "Trimble TSIP",	/* full name of type */
    .trigger        = NULL,		/* no trigger */
    .channels       = TSIP_CHANNELS,	/* consumer-grade GPS */
    .probe          = NULL,		/* no probe */
    .initializer    = tsip_initializer,	/* initialization */
    .get_packet     = packet_get,	/* use the generic packet getter */
    .parse_packet   = tsip_parse_input,	/* parse message packets */
    .rtcm_writer    = NULL,		/* doesn't accept DGPS corrections */
    .speed_switcher = tsip_speed_switch,/* change baud rate */
    .mode_switcher  = NULL,		/* no mode switcher */
    .rate_switcher  = NULL,		/* no rate switcher */
    .cycle_chars    = -1,		/* not relevant, no rate switcher */
    .wrapup         = tsip_wrapup,	/* restore comms parameters */
    .cycle          = 1,		/* updates every second */
};

#endif /* TSIP_ENABLE */
