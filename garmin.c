/*
 * Handle the Garmin binary packet format supported by the USB Garmins
 * tested with the Garmin 18 and other models.  This driver is NOT for
 * serial port connected Garmins, they provide adequate NMEA support.
 *
 * This code is partly from the Garmin IOSDK and partly from the
 * sample code in the Linux garmin_gps driver.
 *
 * Presently this code needs the Linux garmin_gps driver and will
 * not function without it.  It also depends on the Intel byte order
 * (little-endian) so will not work on PPC or other big-endian machines
 *
 * Protocol info from:
 *	 GPS18_TechnicalSpecification.pdf
 *	 iop_spec.pdf
 * http://www.garmin.com/support/commProtocol.html
 *
 * bad code by: Gary E. Miller <gem@rellim.com>
 * all rights abandoned, a thank would be nice if you use this code.
 *
 * -D 3 = packet trace
 * -D 4 = packet details
 * -D 5 = more packet details
 *
 * limitations:
 *
 * do not have from garmin:
 *      pdop
 *      hdop
 *      vdop
 *	magnetic variation
 *
 * known bugs:
 *      hangs in the fread loop instead of keeping state and returning.
 *      may or may not work on a little-endian machine
 */

#define __USE_POSIX199309 1
#include <time.h> // for nanosleep()

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <string.h>
#include <unistd.h>
#include <errno.h>

#if defined (HAVE_SYS_SELECT_H)
#include <sys/select.h>
#endif

#if defined(HAVE_STRINGS_H)
#include <strings.h>
#endif

#include "config.h"
#include "gpsd.h"
#include "gps.h"

#ifdef GARMIN_ENABLE

#define GARMIN_LAYERID_TRANSPORT (unsigned char)  0
#define GARMIN_LAYERID_APPL      (unsigned int) 20
// Linux Garmin USB driver layer-id to use for some control mechanisms
#define GARMIN_LAYERID_PRIVATE  0x01106E4B

// packet ids used in private layer
#define PRIV_PKTID_SET_DEBUG    1
#define PRIV_PKTID_SET_MODE     2
#define PRIV_PKTID_INFO_REQ     3
#define PRIV_PKTID_INFO_RESP    4
#define PRIV_PKTID_RESET_REQ    5
#define PRIV_PKTID_SET_DEF_MODE 6

#define MODE_NATIVE          0
#define MODE_GARMIN_SERIAL   1

#define GARMIN_PKTID_TRANSPORT_START_SESSION_REQ 5
#define GARMIN_PKTID_TRANSPORT_START_SESSION_RESP 6

#define GARMIN_PKTID_PROTOCOL_ARRAY     253
#define GARMIN_PKTID_PRODUCT_RQST       254
#define GARMIN_PKTID_PRODUCT_DATA       255
#define GARMIN_PKTID_PVT_DATA           51
#define GARMIN_PKTID_SAT_DATA           114

#define GARMIN_PKTID_L001_XFER_CMPLT     12
#define GARMIN_PKTID_L001_COMMAND_DATA   10
#define GARMIN_PKTID_L001_DATE_TIME_DATA 14
#define GARMIN_PKTID_L001_RECORDS        27
#define GARMIN_PKTID_L001_WPT_DATA       35

#define	CMND_ABORT			 0
#define	CMND_START_PVT_DATA		 49
#define	CMND_STOP_PVT_DATA		 50
#define	CMND_START_RM_DATA		 110

#define MAX_BUFFER_SIZE 4096

#define GARMIN_CHANNELS	12

// something magic about 64, garmin driver will not return more than
// 64 at a time.  If you read less than 64 bytes the next read will
// just get the last of the 64 byte buffer.
#define ASYNC_DATA_SIZE 64


#pragma pack(1)
// This is the data format of the satellite data from the garmin USB
typedef struct {
	unsigned char  svid;
	unsigned short snr; // 0 - 0xffff
	unsigned char  elev;
	unsigned short azmth;
	unsigned char  status; // bit 0, has ephemeris, 1, has diff correction
                               // bit 2 used in solution
			       // bit 3??
} cpo_sat_data;

/* Garmin D800_Pvt_Date_Type */
// This is the data format of the position data from the garmin USB
typedef struct {
	float alt;  /* altitude above WGS 84 (meters) */
	float epe;  /* estimated position error, 2 sigma (meters)  */
	float eph;  /* epe, but horizontal only (meters) */
	float epv;  /* epe but vertical only (meters ) */
	short	fix; /* 0 - failed integrity check
                      * 1 - invalid or unavailable fix
                      * 2 - 2D
                      * 3 - 3D
		      * 4 - 2D Diff
                      * 5 - 3D Diff
                      */
	double	gps_tow; /* gps time  os week (seconds) */
	double	lat;     /* ->latitude (radians) */
	double	lon;     /* ->longitude (radians) */
	float	lon_vel; /* velocity east (meters/second) */
	float	lat_vel; /* velocity north (meters/second) */
	float	alt_vel; /* velocity up (meters/sec) */
	float	msl_hght; /* height of WGS 84 above MSL (meters) */
	short	leap_sec; /* diff between GPS and UTC (seconds) */
	long	grmn_days;
} cpo_pvt_data;

#ifdef __UNUSED__
typedef struct {
	unsigned long cycles;
	double	 pr;
	unsigned short phase;
	char slp_dtct;
	unsigned char snr_dbhz;
	char  svid;
	char valid;
} cpo_rcv_sv_data;

typedef struct {
	double rcvr_tow;
	short	rcvr_wn;
	cpo_rcv_sv_data sv[GARMIN_CHANNELS];
} cpo_rcv_data;
#endif /* __UNUSED__ */

// This is the packet format to/from the Garmin USB
typedef struct {
    unsigned char  mPacketType;
    unsigned char  mReserved1;
    unsigned short mReserved2;
    unsigned short mPacketId;
    unsigned short mReserved3;
    unsigned long  mDataSize;
    union {
	    char chars[MAX_BUFFER_SIZE];
	    unsigned char uchars[MAX_BUFFER_SIZE];
            cpo_pvt_data pvt;
            cpo_sat_data sats;
    } mData;
} Packet_t;

// useful funcs to read/write ints, only tested on Intel byte order
//  floats and doubles are Intel order only...
static inline void set_int(unsigned char *buf, unsigned int value)
{
        buf[0] = (unsigned char)(0x0FF & value);
        buf[1] = (unsigned char)(0x0FF & (value >> 8));
        buf[2] = (unsigned char)(0x0FF & (value >> 16));
        buf[3] = (unsigned char)(0x0FF & (value >> 24));
}

static inline unsigned short get_short(const unsigned char *buf)
{
        return  (unsigned short)(0xFF & buf[0]) 
		| (unsigned short)((0xFF & buf[1]) << 8);
}

static inline unsigned int get_int(const unsigned char *buf)
{
        return  (unsigned int)(0xFF & buf[0]) | ((0xFF & buf[1]) << 8) | ((0xFF & buf[2]) << 16) | ((0xFF & buf[3]) << 24);
}

// convert radians to degrees
static inline double  radtodeg( double rad) {
	return (double)(rad * RAD_2_DEG );
}

static gps_mask_t PrintPacket(struct gps_device_t *session, Packet_t *pkt );
static void SendPacket (struct gps_device_t *session, Packet_t *aPacket );
static int GetPacket (struct gps_device_t *session );

/*@ -branchstate @*/
// For debugging, decodes and prints some known packets.
static gps_mask_t PrintPacket(struct gps_device_t *session, Packet_t *pkt)
{
    gps_mask_t mask = 0;
    int maj_ver;
    int min_ver;
    unsigned int mode = 0;
    unsigned short prod_id = 0;
    unsigned short ver = 0;
    unsigned int veri = 0;
    time_t time_l = 0;
    unsigned int serial;
    cpo_sat_data *sats = NULL;
    cpo_pvt_data *pvt = NULL;
    int i = 0, j = 0;
    double track;
    char *msg = NULL;
    char buf[40] = "";

    gpsd_report(3, "PrintPacket()\n");
    if ( 4096 < pkt->mDataSize) {
	gpsd_report(3, "bogus packet, size too large=%d\n", pkt->mDataSize);
	return 0;
    }

    (void)snprintf(session->gpsdata.tag, sizeof(session->gpsdata.tag), "%u"
	, (unsigned int)pkt->mPacketType);
    switch ( pkt->mPacketType ) {
    case GARMIN_LAYERID_TRANSPORT:
	switch( pkt->mPacketId ) {
	case GARMIN_PKTID_TRANSPORT_START_SESSION_REQ:
	    gpsd_report(3, "Transport, Start Session req\n");
	    break;
	case GARMIN_PKTID_TRANSPORT_START_SESSION_RESP:
	    mode = get_int(&pkt->mData.uchars[0]);
	    gpsd_report(3, "Transport, Start Session resp, unit: 0x%x\n"
		, mode);
	    break;
	default:
	    gpsd_report(3, "Transport, Packet: Type %d %d %d, ID: %d, Sz: %d\n"
			, pkt->mPacketType
			, pkt->mReserved1
			, pkt->mReserved2
			, pkt->mPacketId
			, pkt->mDataSize);
	    break;
	}
	break;
    case GARMIN_LAYERID_APPL:
	switch( pkt->mPacketId ) {
	case GARMIN_PKTID_L001_COMMAND_DATA:
	    prod_id = get_short(&pkt->mData.uchars[0]);
            switch ( prod_id ) {
	    case CMND_ABORT:
		msg = "Abort current xfer";
	  	break;
	    case CMND_START_PVT_DATA:
		msg = "Start Xmit PVT data";
	  	break;
	    case CMND_STOP_PVT_DATA:
		msg = "Stop Xmit PVT data";
	  	break;
	    case CMND_START_RM_DATA:
		msg = "Start RMD data";
	  	break;
	    default:
		(void)snprintf(buf, sizeof(buf), "Unknown: %u", prod_id);
		msg = buf;
	        break;
            }
	    gpsd_report(3, "Appl, Command Data: %s\n", msg);
	    break;
	case GARMIN_PKTID_PRODUCT_RQST:
	    gpsd_report(3, "Appl, Product Data req\n");
	    break;
	case GARMIN_PKTID_PRODUCT_DATA:
	    prod_id = get_short(&pkt->mData.uchars[0]);
	    ver = get_short(&pkt->mData.uchars[2]);
	    maj_ver = ver / 100;
	    min_ver = ver - (maj_ver * 100);
	    gpsd_report(3, "Appl, Product Data, sz: %d\n"
			, pkt->mDataSize);
	    gpsd_report(1, "Garmin Product ID: %d, SoftVer: %d.%02d\n"
			, prod_id, maj_ver, min_ver);
	    gpsd_report(1, "Garmin Product Desc: %s\n"
			, &pkt->mData.chars[4]);
	    break;
	case GARMIN_PKTID_PVT_DATA:
	    gpsd_report(3, "Appl, PVT Data Sz: %d\n", pkt->mDataSize);

	    pvt = &pkt->mData.pvt;

	    // 631065600, unix seconds for 31 Dec 1989 Zulu 
	    time_l = (time_t)(631065600 + (pvt->grmn_days * 86400));
	    time_l -= pvt->leap_sec;
	    session->context->leap_seconds = pvt->leap_sec;
	    session->context->valid = LEAP_SECOND_VALID;
	    // gps_tow is always like x.999 or x.998 so just round it
	    time_l += (time_t) round(pvt->gps_tow);
	    session->gpsdata.newdata.time 
		= session->gpsdata.sentence_time 
		= (double)time_l;
	    gpsd_report(5, "time_l: %ld\n", (long int)time_l);

	    session->gpsdata.newdata.latitude = radtodeg(pvt->lat);
	    session->gpsdata.newdata.longitude = radtodeg(pvt->lon);

	    // altitude over WGS84 converted to MSL
	    session->gpsdata.newdata.altitude = pvt->alt + pvt->msl_hght;

	    // geoid separation from WGS 84
            // gpsd sign is opposite of garmin sign
	    session->gpsdata.separation = -pvt->msl_hght;

	    // Estimated position error in meters.
	    session->gpsdata.epe = pvt->epe * (GPSD_CONFIDENCE/2);
	    session->gpsdata.newdata.eph = pvt->eph * (GPSD_CONFIDENCE/2);
	    session->gpsdata.newdata.epv = pvt->epv * (GPSD_CONFIDENCE/2);

	    // convert lat/lon to knots
	    session->gpsdata.newdata.speed
		= hypot(pvt->lon_vel, pvt->lat_vel) * 1.9438445;

            // keep climb in meters/sec
	    session->gpsdata.newdata.climb = pvt->alt_vel;

	    track = atan2(pvt->lon_vel, pvt->lat_vel);
	    if (track < 0) {
		track += 2 * PI;
	    }
	    session->gpsdata.newdata.track = radtodeg(track);

	    switch ( pvt->fix) {
	    case 0:
	    case 1:
	    default:
		// no fix
		session->gpsdata.status = STATUS_NO_FIX;
		session->gpsdata.newdata.mode = MODE_NO_FIX;
		break;
	    case 2:
		// 2D fix
		session->gpsdata.status = STATUS_FIX;
		session->gpsdata.newdata.mode = MODE_2D;
		break;
	    case 3:
		// 3D fix
		session->gpsdata.status = STATUS_FIX;
		session->gpsdata.newdata.mode = MODE_3D;
		break;
	    case 4:
		// 2D Differential fix
		session->gpsdata.status = STATUS_DGPS_FIX;
		session->gpsdata.newdata.mode = MODE_2D;
		break;
	    case 5:
		// 3D differential fix
		session->gpsdata.status = STATUS_DGPS_FIX;
		session->gpsdata.newdata.mode = MODE_3D;
		break;
	    }
#ifdef NTPSHM_ENABLE
	    if (session->gpsdata.newdata.mode > MODE_NO_FIX)
		(void) ntpshm_put(session, session->gpsdata.newdata.time);
#endif /* NTPSHM_ENABLE */

	    gpsd_report(4, "Appl, mode %d, status %d\n"
			, session->gpsdata.newdata.mode
			, session->gpsdata.status);

	    gpsd_report(3, "UTC Time: %lf\n", session->gpsdata.newdata.time);
	    gpsd_report(3, "Geoid Separation (MSL - WGS84): from garmin %lf, calculated %lf\n"
		, -pvt->msl_hght
		, wgs84_separation(session->gpsdata.newdata.latitude
			, session->gpsdata.newdata.longitude));
	    gpsd_report(3, "Alt: %.3f, Epe: %.3f, Eph: %.3f, Epv: %.3f, Fix: %d, Gps_tow: %f, Lat: %.3f, Lon: %.3f, LonVel: %.3f, LatVel: %.3f, AltVel: %.3f, MslHgt: %.3f, Leap: %d, GarminDays: %ld\n"
			, pvt->alt
			, pvt->epe
			, pvt->eph
			, pvt->epv
			, pvt->fix
			, pvt->gps_tow
			, session->gpsdata.newdata.latitude
			, session->gpsdata.newdata.longitude
			, pvt->lon_vel
			, pvt->lat_vel
			, pvt->alt_vel
			, pvt->msl_hght
			, pvt->leap_sec
			, pvt->grmn_days);

	    mask |= TIME_SET | LATLON_SET | ALTITUDE_SET | STATUS_SET | MODE_SET | SPEED_SET | TRACK_SET | CLIMB_SET | HERR_SET | VERR_SET | PERR_SET | CYCLE_START_SET;
	    break;
	case GARMIN_PKTID_SAT_DATA:
	    gpsd_report(3, "Appl, SAT Data Sz: %d\n", pkt->mDataSize);
	    sats = &pkt->mData.sats;

	    session->gpsdata.satellites_used = 0;
	    memset(session->gpsdata.used,0,sizeof(session->gpsdata.used));
	    gpsd_zero_satellites(&session->gpsdata);
	    for ( i = 0, j = 0 ; i < GARMIN_CHANNELS ; i++, sats++ ) {
		gpsd_report(4,
			    "  Sat %d, snr: %d, elev: %d, Azmth: %d, Stat: %x\n"
			    , sats->svid
			    , sats->snr
			    , sats->elev
			    , sats->azmth
			    , sats->status);

		if ( 255 == (int)sats->svid ) {
		    // Garmin uses 255 for empty
		    // gpsd uses 0 for empty
		    continue;
		}

		session->gpsdata.PRN[j]       = (int)sats->svid;
		session->gpsdata.azimuth[j]   = (int)sats->azmth;
		session->gpsdata.elevation[j] = (int)sats->elev;
		// snr units??
		// garmin 0 -> 0xffff, NMEA 99 -> 0
		session->gpsdata.ss[j]
		    = 99 - (int)((100 *( unsigned long)sats->snr) >> 16);
		if ( (unsigned char)0 != (sats->status & 4 ) )  {
		    // used in solution?
		    session->gpsdata.used[session->gpsdata.satellites_used++]
			= (int)sats->svid;
		}
		session->gpsdata.satellites++;
		j++;
	    }
	    mask |= SATELLITE_SET | USED_SET;
	    break;
	case GARMIN_PKTID_PROTOCOL_ARRAY:
            // this packet is never requested, it just comes, in some case
            // after a GARMIN_PKTID_PRODUCT_RQST 
	    gpsd_report(3, "Appl, Product Capability, sz: %d\n", pkt->mDataSize);
            for ( i = 0; i < (int)pkt->mDataSize ; i += 3 ) {
		    gpsd_report(3, "  %c%03d\n", pkt->mData.chars[i]
			, get_short( &(pkt->mData.uchars[i+1])) );
	    }
	    break;
	default:
	    gpsd_report(3, "Appl, ID: %d, Sz: %d\n"
			, pkt->mPacketId
			, pkt->mDataSize);
	    break;
	}
	break;
    case 75:
	// private
	switch( pkt->mPacketId ) {
	case PRIV_PKTID_SET_MODE:
	    prod_id = (unsigned short)get_int(&pkt->mData.uchars[0]);
	    gpsd_report(3, "Private, Set Mode: %d\n", prod_id);
	    break;
	case PRIV_PKTID_INFO_REQ:
	    gpsd_report(3, "Private, ID: Info Req\n");
	    break;
	case PRIV_PKTID_INFO_RESP:
	    veri = get_int(pkt->mData.uchars);
	    maj_ver = (int)(veri >> 16);
	    min_ver = (int)(veri & 0xffff);
	    mode = get_int(&pkt->mData.uchars[4]);
	    serial = get_int(&pkt->mData.uchars[8]);
	    gpsd_report(3, "Private, ID: Info Resp\n");
	    gpsd_report(1, "Garmin USB Driver found, Version %d.%d, Mode: %d, GPS Serial# %u\n"
			,  maj_ver, min_ver, mode, serial);
	    break;
	default:
	    gpsd_report(3, "Private, Packet: ID: %d, Sz: %d\n"
			, pkt->mPacketId
			, pkt->mDataSize);
	    break;
	}
	break;
    default:
	gpsd_report(3, "Packet: Type %d %d %d, ID: %d, Sz: %d\n"
		    , pkt->mPacketType
		    , pkt->mReserved1
		    , pkt->mReserved2
		    , pkt->mPacketId
		    , pkt->mDataSize);
	break;
    }

    return mask;
}
/*@ +branchstate @*/

//-----------------------------------------------------------------------------
// send a packet in GarminUSB format
static void SendPacket (struct gps_device_t *session, Packet_t *aPacket ) 
{
	size_t theBytesToWrite = (size_t)(12 + aPacket->mDataSize);
	ssize_t theBytesReturned = 0;

        gpsd_report(4, "SendPacket(), writing %d bytes\n", theBytesToWrite);
        (void)PrintPacket ( session,  aPacket);

	theBytesReturned = write( session->gpsdata.gps_fd
		    , aPacket, theBytesToWrite);
	gpsd_report(4, "SendPacket(), wrote %d bytes\n", theBytesReturned);

	// Garmin says:
	// If the packet size was an exact multiple of the USB packet
	// size, we must make a final write call with no data

	// as a practical matter no known pckets are 64 bytes long so
        // this is untested

	// So here goes just in case
	if( 0 == (theBytesToWrite % ASYNC_DATA_SIZE) ) {
		char *n = "";
		theBytesReturned = write( session->gpsdata.gps_fd
		    , &n, 0);
	}
}

//-----------------------------------------------------------------------------
// Gets a single packet.
// this is odd, the garmin usb driver will only return 64 bytes, or less
// at a time, no matter what you ask for.
//
// is you ask for less than 64 bytes then the next packet will include
// just the remaining bytes of the last 64 byte packet.
//
// Reading a packet of length Zero, or less than 64, signals the end of 
// the entire packet.
//
// The Garmin sample WinXX code also assumes the same behavior, so
// maybe it is something in the USB protocol.
//
// Return: 0 = got a good packet
//         -1 = error
//         1 = got partial packet
static int GetPacket (struct gps_device_t *session ) 
{
    struct timespec delay, rem;
    int cnt = 0;
    // int x = 0; // for debug dump

    memset( session->driver.garmin.Buffer, 0, sizeof(Packet_t));
    memset( &delay, 0, sizeof(delay));
    session->driver.garmin.BufferLen = 0;
    session->outbuflen = 0;

    gpsd_report(4, "GetPacket()\n");

    for( cnt = 0 ; cnt < 10 ; cnt++ ) {
	// Read async data until the driver returns less than the
	// max async data size, which signifies the end of a packet

	// not optimal, but given the speed and packet nature of
	// the USB not too bad for a start
	ssize_t theBytesReturned = 0;
	unsigned char *buf = session->driver.garmin.Buffer;

	theBytesReturned = read(session->gpsdata.gps_fd
		, buf + session->driver.garmin.BufferLen
		, ASYNC_DATA_SIZE);
        if ( 0 >  theBytesReturned ) {
	    // read error...
            // or EAGAIN, but O_NONBLOCK is never set
	    gpsd_report(0, "GetPacket() read error=%d, errno=%d\n"
		, theBytesReturned, errno);
	    continue;
	}
	gpsd_report(5, "got %d bytes\n", theBytesReturned);

	session->driver.garmin.BufferLen += theBytesReturned;
	if ( 64 > theBytesReturned ) {
	    // zero length, or short, read is a flag for got the whole packet
            break;
	}
		
	if ( 256 <=  session->driver.garmin.BufferLen ) {
	    // really bad read error...
	    session->driver.garmin.BufferLen = 0;
	    gpsd_report(3, "GetPacket() packet too long!\n");
	    break;
	}

	/*@ ignore @*/
	delay.tv_sec = 0;
	delay.tv_nsec = 3330000L;
	while (nanosleep(&delay, &rem) < 0)
	    continue;
	/*@ end @*/
    }
    // dump the individual bytes, debug only
    // for ( x = 0; x < session->driver.garmin.BufferLen; x++ ) {
        // gpsd_report(6, "p[%d] = %x\n", x, session->driver.garmin.Buffer[x]);
    // }
    if ( 10 <= cnt ) {
	    gpsd_report(3, "GetPacket() packet too long or too slow!\n");
	    return -1;
    }

    gpsd_report(5, "GotPacket() sz=%d \n", session->driver.garmin.BufferLen);
    session->outbuflen = session->driver.garmin.BufferLen;
    return 0;
}

/*
 * garmin_probe()
 *
 * return 1 if garmin_gps device found
 * return 0 if not
 */
static bool garmin_probe(struct gps_device_t *session)
{

    Packet_t *thePacket = NULL;
    unsigned char *buffer = NULL;
    fd_set fds, rfds;
    struct timeval tv;
    int sel_ret = 0;
    int ok = 0;
    int i;

    /* check for USB serial drivers -- very Linux-specific */
    if (access("/sys/module/garmin_gps", R_OK) != 0) {
	gpsd_report(5, "garmin_gps not active.\n"); 
        return false;
    }

    /* Save original terminal parameters */
    if (tcgetattr(session->gpsdata.gps_fd,&session->ttyset_old) != 0) {
	gpsd_report(0, "garmin_probe: error getting port attributes: %s\n",
             strerror(errno));
	return false;
    }
    memcpy(&session->ttyset,&session->ttyset_old,sizeof(session->ttyset));

    (void)cfmakeraw(&session->ttyset);

    if (tcsetattr( session->gpsdata.gps_fd, TCIOFLUSH, &session->ttyset) < 0) {
	gpsd_report(0, "garmin_probe: error changing port attributes: %s\n",
             strerror(errno));
	return false;
    }

    /* reset the buffer and buffer length */
    memset( session->driver.garmin.Buffer, 0, sizeof(session->driver.garmin.Buffer) );
    session->driver.garmin.BufferLen = 0;

    if (sizeof(session->driver.garmin.Buffer) < sizeof(Packet_t)) {
	gpsd_report(0, "garmin_probe: Compile error, garmin.Buffer too small.\n",
             strerror(errno));
	return false;
    }

    buffer = session->driver.garmin.Buffer;
    thePacket = (Packet_t*)buffer;

    // set Mode 0
    set_int(buffer, GARMIN_LAYERID_PRIVATE);
    set_int(buffer+4, PRIV_PKTID_SET_MODE);
    set_int(buffer+8, 4); // data length 4
    set_int(buffer+12, 0); // mode 0

    gpsd_report(3, "Set garmin_gps driver mode = 0\n");
    SendPacket( session,  thePacket);
    // expect no return packet !?

    // get Version info
    gpsd_report(3, "Get garmin_gps driver version\n");
    set_int(buffer, GARMIN_LAYERID_PRIVATE);
    set_int(buffer+4, PRIV_PKTID_INFO_REQ);
    set_int(buffer+8, 0); // data length 0

    SendPacket(session,  thePacket);

    /* get and print the driver Version info */

    FD_ZERO(&fds); 
    FD_SET(session->gpsdata.gps_fd, &fds);

    /* Wait, nicely, until the device returns the Version info
     * Toss any other packets, up to 4 */
    ok = 0;
    memset( &tv,0,sizeof(tv));
    for( i = 0 ; i < 4 ; i++ ) {
        memcpy((char *)&rfds, (char *)&fds, sizeof(rfds));

	tv.tv_sec = 1; tv.tv_usec = 0;
	sel_ret = select(FD_SETSIZE, &rfds, NULL, NULL, &tv);
	if (sel_ret < 0) {
	    if (errno == EINTR)
		continue;
	    gpsd_report(0, "select: %s\n", strerror(errno));
	    return false;
	} else if ( sel_ret == 0 ) {
	    gpsd_report(3, "garmin_probe() timeout, INFO_REQ\n");
	    // restore old terminal settings
            (void)tcsetattr(session->gpsdata.gps_fd, TCIOFLUSH
		, &session->ttyset_old);
	    return false;
        }
	if ( 0 == GetPacket( session ) ) {
	    (void)PrintPacket(session, thePacket);

	    if( ( (unsigned char)75 == thePacket->mPacketType)
	        && (PRIV_PKTID_INFO_RESP == thePacket->mPacketId) ) {
                ok = 1;
	        break;
	    }
	}
    }

    if ( 0 == ok ) {
	gpsd_report(2, "Garmin driver never answered to INFO_REQ.\n");
	// restore old terminal settings
        (void)tcsetattr(session->gpsdata.gps_fd, TCIOFLUSH
		, &session->ttyset_old);
	return false;
    }
    /* depending on the GARMIN version, the device may spontaneously
       return the Product Capability here */

    /* Tell the device that we are starting a session. */
    gpsd_report(3, "Send Garmin Start Session\n");

    set_int(buffer, GARMIN_LAYERID_TRANSPORT);
    set_int(buffer+4, GARMIN_PKTID_TRANSPORT_START_SESSION_REQ);
    set_int(buffer+8, 0); // data length 0

    SendPacket(session,  thePacket);

    /* Wait until the device is ready to the start the session
     * Toss any other packets, up to 4 */
    ok = 0;
    for( i = 0 ; i < 4 ; i++ ) {
        memcpy((char *)&rfds, (char *)&fds, sizeof(rfds));

	tv.tv_sec = 1; tv.tv_usec = 0;
	sel_ret = select(FD_SETSIZE, &rfds, NULL, NULL, &tv);
	if (sel_ret < 0) {
	    if (errno == EINTR)
		continue;
	    gpsd_report(0, "select: %s\n", strerror(errno));
	    return(0);
	} else if ( sel_ret == 0 ) {
	    gpsd_report(3, "garmin_probe() timeout, START_SESSION\n");
	    // restore old terminal settings
            (void)tcsetattr(session->gpsdata.gps_fd, TCIOFLUSH
		, &session->ttyset_old);
	    return(0);
        }
	if ( 0 == GetPacket( session ) ) {
	    gpsd_report(3, "Got packet waiting for START_SESSION\n");
	    (void)PrintPacket(session, thePacket);

	    if( (GARMIN_LAYERID_TRANSPORT == thePacket->mPacketType)
	        && (GARMIN_PKTID_TRANSPORT_START_SESSION_RESP
		    == thePacket->mPacketId) ) {
                ok = 1;
	        break;
	    }
	}
    }

    if ( 0 == ok ) {
	gpsd_report(2, "Garmin driver never answered to START_SESSION.\n");
	// restore old terminal settings
        (void)tcsetattr(session->gpsdata.gps_fd, TCIOFLUSH
		, &session->ttyset_old);
	return false;
    }

    // Tell the device to send product data
    gpsd_report(3, "Get Garmin Product Data\n");

    set_int(buffer, GARMIN_LAYERID_APPL);
    set_int(buffer+4, GARMIN_PKTID_PRODUCT_RQST);
    set_int(buffer+8, 0); // data length 0

    SendPacket(session,  thePacket);

    // Get the product data packet
    // Toss any other packets, up to 4
    ok = 0;
    for( i = 0 ; i < 4 ; i++ ) {
        memcpy((char *)&rfds, (char *)&fds, sizeof(rfds));

	tv.tv_sec = 1; tv.tv_usec = 0;
	sel_ret = select(FD_SETSIZE, &rfds, NULL, NULL, &tv);
	if (sel_ret < 0) {
	    if (errno == EINTR)
		continue;
	    gpsd_report(0, "select: %s\n", strerror(errno));
	    return false;
	} else if ( sel_ret == 0 ) {
	    gpsd_report(3, "garmin_probe() timeout, PRODUCT_DATA\n");
	    // restore old terminal settings
            (void)tcsetattr(session->gpsdata.gps_fd, TCIOFLUSH
		, &session->ttyset_old);
	    return false;
        }
	if ( 0 == GetPacket( session ) ) {
	    (void)PrintPacket(session, thePacket);

	    if( (GARMIN_LAYERID_APPL == (unsigned int)thePacket->mPacketType)
	        && ( GARMIN_PKTID_PRODUCT_DATA == thePacket->mPacketId) ) {
    		ok = 1;
	        break;
	    }
	}
    }

    if ( 0 == ok ) {
	gpsd_report(2, "Garmin driver never answered to PRODUCT_DATA.\n");
	// restore old terminal settings
        (void)tcsetattr(session->gpsdata.gps_fd, TCIOFLUSH
		, &session->ttyset_old);
	return false;
    }
    return true;
}

/*
 * garmin_init()
 *
 * init a garmin_gps device,
 * session->gpsdata.gps_fd is assumed to already be open.
 *
 * the garmin_gps driver ignores all termios, baud rates, etc. so
 * any twiddling of that previously done is harmless.
 *
 */
static void garmin_init(struct gps_device_t *session)
{
	unsigned char *buffer = session->driver.garmin.Buffer;
	Packet_t *thePacket = (Packet_t*)buffer;
	bool ret;

	gpsd_report(5, "to garmin_probe()\n");
	ret = garmin_probe( session );
        /* FIXME - what if return code was bad */
        /* FIXME - return code is always bad */
	gpsd_report(3, "from garmin_probe() = %d\n", (int)ret);

	// turn on PVT data 49
	gpsd_report(3, "Set Garmin to send reports every 1 second\n");

	set_int(buffer, GARMIN_LAYERID_APPL);
	set_int(buffer+4, GARMIN_PKTID_L001_COMMAND_DATA);
	set_int(buffer+8, 2); // data length 2
	set_int(buffer+12, CMND_START_PVT_DATA);

	SendPacket(session,  thePacket);

	// turn on RMD data 110
	//set_int(buffer, GARMIN_LAYERID_APPL);
	//set_int(buffer+4, GARMIN_PKTID_L001_COMMAND_DATA);
	//set_int(buffer+8, 2); // data length 2
	//set_int(buffer+12, CMND_START_RM_DATA);

	//SendPacket(session,  thePacket);
}

static void garmin_close(struct gps_device_t *session UNUSED) 
{
    /* FIXME -- do we need to put the garmin to sleep?  or is closing the port
       sufficient? */
    gpsd_report(3, "garmin_close()\n");
    return;
}

static ssize_t garmin_get_packet(struct gps_device_t *session) 
{
    return (ssize_t)( 0 == GetPacket( session ) ? 1 : 0);
}

static gps_mask_t garmin_parse_input(struct gps_device_t *session)
{
    gpsd_report(5, "garmin_parse_input()\n");
    return PrintPacket(session, (Packet_t*)session->driver.garmin.Buffer);
}

/* this is everything we export */
struct gps_type_t garmin_binary =
{
    .typename       = "Garmin binary",	/* full name of type */
    .trigger        = NULL,		/* no trigger, it has a probe */
    .channels       = GARMIN_CHANNELS,	/* consumer-grade GPS */
    .probe          = garmin_probe,	/* how to detect at startup time */
    .initializer    = garmin_init,	/* initialize the device */
    .get_packet     = garmin_get_packet,/* how to grab a packet */
    .parse_packet   = garmin_parse_input,	/* parse message packets */
    .rtcm_writer    = NULL,		/* don't send DGPS corrections */
    .speed_switcher = NULL,		/* no speed switcher */
    .mode_switcher  = NULL,		/* no mode switcher */
    .rate_switcher  = NULL,		/* no sample-rate switcher */
    .cycle_chars    = -1,		/* not relevant, no rate switch */
    .wrapup         = garmin_close,	/* close hook */
    .cycle          = 1,		/* updates every second */
};

#endif /* GARMIN_ENABLE */

