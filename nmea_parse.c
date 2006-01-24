#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

#include "gpsd.h"
#include "timebase.h"

#ifdef NMEA_ENABLE
/**************************************************************************
 *
 * Parser helpers begin here
 *
 **************************************************************************/

static void do_lat_lon(char *field[], struct gps_data_t *out)
/* process a pair of latitude/longitude fields starting at field index BEGIN */
{
    double lat, lon, d, m;
    char str[20], *p;
    int updated = 0;

    if (*(p = field[0]) != '\0') {
	strncpy(str, p, 20);
	(void)sscanf(p, "%lf", &lat);
	m = 100.0 * modf(lat / 100.0, &d);
	lat = d + m / 60.0;
	p = field[1];
	if (*p == 'S')
	    lat = -lat;
	if (out->newdata.latitude != lat)
	    out->newdata.latitude = lat;
	updated++;
    }
    if (*(p = field[2]) != '\0') {
	strncpy(str, p, 20);
	(void)sscanf(p, "%lf", &lon);
	m = 100.0 * modf(lon / 100.0, &d);
	lon = d + m / 60.0;

	p = field[3];
	if (*p == 'W')
	    lon = -lon;
	if (out->newdata.longitude != lon)
	    out->newdata.longitude = lon;
	updated++;
    }
}

/**************************************************************************
 *
 * Scary timestamp fudging begins here
 *
 * Four sentences, GGA and GLL and RMC and ZDA, contain timestamps.
 * Timestamps always look like hhmmss.ss, with the trailing .ss part
 * optional.  RMC has a date field, in the format ddmmyy.  ZDA has
 * separate fields for day/month/year, with a 4-digit year.  This
 * means that for RMC we must supply a century and for GGA and GLL we
 * must supply a century, year, and day.  We get the missing data from
 * a previous RMC or ZDA; century in RMC is supplied by a constant if 
 * there has been no previous RMC.
 *
 **************************************************************************/

#define DD(s)	((int)((s)[0]-'0')*10+(int)((s)[1]-'0'))

static void merge_ddmmyy(char *ddmmyy, struct gps_device_t *session)
/* sentence supplied ddmmyy, but no century part */
{
    if (session->driver.nmea.date.tm_year == 0)
	session->driver.nmea.date.tm_year = (CENTURY_BASE + DD(ddmmyy+4)) - 1900;
    session->driver.nmea.date.tm_mon = DD(ddmmyy+2)-1;
    session->driver.nmea.date.tm_mday = DD(ddmmyy);
}

static void merge_hhmmss(char *hhmmss, struct gps_device_t *session)
/* update from a UTC time */
{
    int old_hour = session->driver.nmea.date.tm_hour;

    session->driver.nmea.date.tm_hour = DD(hhmmss);
	if (session->driver.nmea.date.tm_hour < old_hour)	/* midnight wrap */ 
	session->driver.nmea.date.tm_mday++;
    session->driver.nmea.date.tm_min = DD(hhmmss+2);
    session->driver.nmea.date.tm_sec = DD(hhmmss+4);
    session->driver.nmea.subseconds = atof(hhmmss+4) - session->driver.nmea.date.tm_sec;
}

#undef DD

/**************************************************************************
 *
 * NMEA sentence handling begins here
 *
 **************************************************************************/

static gps_mask_t processGPRMC(int count, char *field[], struct gps_device_t *session)
/* Recommend Minimum Course Specific GPS/TRANSIT Data */
{
    /*
        RMC,225446.33,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E,A*68
           225446.33    Time of fix 22:54:46 UTC
           A            Status of Fix A = Autonomous, valid; D = Differential, valid; V = invalid
           4916.45,N    Latitude 49 deg. 16.45 min North
           12311.12,W   Longitude 123 deg. 11.12 min West
           000.5        Speed over ground, Knots
           054.7        Course Made Good, True north
           191194       Date of fix  19 November 1994
           020.3,E      Magnetic variation 20.3 deg East
	   A            FAA mode indicator (NMEA 2.3 and later)
                        A=autonomous, D=differential, E=Estimated,
                        N=not valid, S=Simulator, M=Manual input mode
           *68          mandatory nmea_checksum

     * SiRF chipsets don't return either Mode Indicator or magnetic variation.
     */
    gps_mask_t mask = 0;

    if (strcmp(field[2], "V")==0) {
	/* copes with Magellan EC-10X, see below */
	if (session->gpsdata.status != STATUS_NO_FIX) {
	    session->gpsdata.status = STATUS_NO_FIX;
	    mask |= STATUS_SET;
	}
	if (session->gpsdata.newdata.mode >= MODE_2D) {
	    session->gpsdata.newdata.mode = MODE_NO_FIX;
	    mask |= MODE_SET;
	}
	/* set something nz, so it won't look like an unknown sentence */
	mask |= ONLINE_SET;
    } else if (strcmp(field[2], "A")==0) {
	if (count > 9) {
	    merge_ddmmyy(field[9], session);
	    merge_hhmmss(field[1], session);
	    mask |= TIME_SET;
	    session->gpsdata.newdata.time = (double)mkgmtime(&session->driver.nmea.date)+session->driver.nmea.subseconds;
	    if (session->gpsdata.sentence_time != session->gpsdata.newdata.time)
		mask |= CYCLE_START_SET;
	    session->gpsdata.sentence_time = session->gpsdata.newdata.time;
	}
	do_lat_lon(&field[3], &session->gpsdata);
	mask |= LATLON_SET;
	session->gpsdata.newdata.speed = atof(field[7]) * KNOTS_TO_MPS;
	session->gpsdata.newdata.track = atof(field[8]);
	mask |= (TRACK_SET | SPEED_SET);
	/*
	 * This copes with GPSes like the Magellan EC-10X that *only* emit
	 * GPRMC. In this case we set mode and status here so the client
	 * code that relies on them won't mistakenly believe it has never
	 * received a newdata.
	 */
	if (session->gpsdata.status == STATUS_NO_FIX) {
	    session->gpsdata.status = STATUS_FIX;	/* could be DGPS_FIX, we can't tell */
	    mask |= STATUS_SET;
	}
	if (session->gpsdata.newdata.mode < MODE_2D) {
	    session->gpsdata.newdata.mode = MODE_2D;
	    mask |= MODE_SET;
	}
    }

    return mask;
}

static gps_mask_t processGPGLL(int count, char *field[], struct gps_device_t *session)
/* Geographic position - Latitude, Longitude */
{
    /* Introduced in NMEA 3.0.  Here are the fields:
     *
     * 1,2 Latitude, N (North) or S (South)
     * 3,4 Longitude, E (East) or W (West)
     * 5 UTC of position
     * 6 A=Active, V=Void
     * 7 Mode Indicator
     *   A = Autonomous mode
     *   D = Differential Mode
     *   E = Estimated (dead-reckoning) mode
     *   M = Manual Input Mode
     *   S = Simulated Mode
     *   N = Data Not Valid
     *
     * I found a note at <http://www.secoh.ru/windows/gps/nmfqexep.txt>
     * indicating that the Garmin 65 does not return time and status.
     * SiRF chipsets don't return the Mode Indicator.
     * This code copes gracefully with both quirks.
     * 
     * Unless you care about the FAA indicator, this sentence supplies nothing
     * that GPRMC doesn't already.  But at least one Garmin GPS -- the 48
     * actually ships updates in GPLL that aren't redundant.
     */
    char *status = field[7];
    gps_mask_t mask = ERROR_SET;

    if (strcmp(field[6], "A")==0 && (count < 8 || *status != 'N')) {
	int newstatus = session->gpsdata.status;

	mask = 0;
	merge_hhmmss(field[5], session);
	if (session->driver.nmea.date.tm_year != 0) 
	    gpsd_report(1, "can't use GGA/GGL time until after ZDA or RMC has supplied a year.\n");
	else {
	    mask = TIME_SET;
	    session->gpsdata.newdata.time = (double)mkgmtime(&session->driver.nmea.date)+session->driver.nmea.subseconds;
	    if (session->gpsdata.sentence_time != session->gpsdata.newdata.time)
		mask |= CYCLE_START_SET;
	    session->gpsdata.sentence_time = session->gpsdata.newdata.time;
	}
	do_lat_lon(&field[1], &session->gpsdata);
	mask |= LATLON_SET;
	if (count >= 8 && *status == 'D')
	    newstatus = STATUS_DGPS_FIX;	/* differential */
	else
	    newstatus = STATUS_FIX;
	session->gpsdata.status = newstatus;
	mask |= STATUS_SET;
	gpsd_report(3, "GPGLL sets status %d\n", session->gpsdata.status);
    }

    return mask;
}

static gps_mask_t processGPGGA(int c UNUSED, char *field[], struct gps_device_t *session)
/* Global Positioning System Fix Data */
{
    /*
        GGA,123519,4807.038,N,01131.324,E,1,08,0.9,545.4,M,46.9,M, , *42
           123519       Fix taken at 12:35:19 UTC
           4807.038,N   Latitude 48 deg 07.038' N
           01131.324,E  Longitude 11 deg 31.324' E
           1            Fix quality: 0 = invalid, 1 = GPS fix, 2 = DGPS fix,
	   		3=PPS (Precise Position Service),
			4=RTK (Real Time Kinematic) with fixed integers,
			5=Float RTK, 6=Estimated, 7=Manual, 8=Simulator
           08           Number of satellites being tracked
           0.9          Horizontal dilution of position
           545.4,M      Altitude, Metres above mean sea level
           46.9,M       Height of geoid (mean sea level) above WGS84
                        ellipsoid, in Meters
           (empty field) time in seconds since last DGPS update
           (empty field) DGPS station ID number (0000-1023)
    */
    gps_mask_t mask;

    session->gpsdata.status = atoi(field[6]);
    gpsd_report(3, "GPGGA sets status %d\n", session->gpsdata.status);
    mask = STATUS_SET;
    if (session->gpsdata.status > STATUS_NO_FIX) {
	char *altitude;
	double oldfixtime = session->gpsdata.newdata.time;

	merge_hhmmss(field[1], session);
	if (session->driver.nmea.date.tm_year == 0) 
	    gpsd_report(1, "can't use GGA/GGL time until after ZDA or RMC has supplied a year.\n");
	else {
	    mask |= TIME_SET;
	    session->gpsdata.newdata.time = (double)mkgmtime(&session->driver.nmea.date)+session->driver.nmea.subseconds;
	}
	do_lat_lon(&field[2], &session->gpsdata);
	mask |= LATLON_SET;
        session->gpsdata.satellites_used = atoi(field[7]);
	altitude = field[9];
	/*
	 * SiRF chipsets up to version 2.2 report a null altitude field.
	 * See <http://www.sirf.com/Downloads/Technical/apnt0033.pdf>.
	 * If we see this, force mode to 2D at most.
	 */
	if (altitude[0] == '\0') {
	    if (session->gpsdata.newdata.mode == MODE_3D) {
		session->gpsdata.newdata.mode = session->gpsdata.status ? MODE_2D : MODE_NO_FIX; 
		mask |= MODE_SET;
	    }
	} else {
	    double oldaltitude = session->gpsdata.newdata.altitude;

	    session->gpsdata.newdata.altitude = atof(altitude);
	    mask |= ALTITUDE_SET;


	    /*
	     * Compute climb/sink in the simplest possible way.
	     * This substitutes for the climb report provided by
	     * SiRF and Garmin chips, which might have some smoothing
	     * going on.
	     */
	    if (isnan(oldaltitude)==0 || session->gpsdata.newdata.time==oldfixtime)
		session->gpsdata.newdata.climb = 0;
	    else {
		session->gpsdata.newdata.climb = (session->gpsdata.newdata.altitude-oldaltitude)/(session->gpsdata.newdata.time-oldfixtime);
	    }
	    mask |= CLIMB_SET;
	}
	if (strlen(field[11]) > 0) {
	   session->gpsdata.separation = atof(field[11]);
	} else {
	   session->gpsdata.separation = wgs84_separation(session->gpsdata.newdata.latitude,session->gpsdata.newdata.longitude);
	}
    }
    return mask;
}

static gps_mask_t processGPGSA(int count, char *field[], struct gps_device_t *session)
/* GPS DOP and Active Satellites */
{
    /*
	eg1. $GPGSA,A,3,,,,,,16,18,,22,24,,,3.6,2.1,2.2*3C
	eg2. $GPGSA,A,3,19,28,14,18,27,22,31,39,,,,,1.7,1.0,1.3*35
	1    = Mode:
	       M=Manual, forced to operate in 2D or 3D
	       A=Automatic, 3D/2D
	2    = Mode: 1=Fix not available, 2=2D, 3=3D
	3-14 = PRNs of satellites used in position fix (null for unused fields)
	15   = PDOP
	16   = HDOP
	17   = VDOP
     */
    gps_mask_t mask;
    int i;

    /*
     * One chipset called the i.Trek M3 issues GPGSA lines that look like
     * this: "$GPGSA,A,1,,,,*32" when it has no fix.  This is broken
     * in at least two ways: it's got the wrong number of fields, and
     * it claims to be a valid sentence (A flag) when it isn't.
     * Alarmingly, it's possible this error may be generic to SiRF-IIIs.
     */
    if (count < 17)
	return ONLINE_SET;

    session->gpsdata.newdata.mode = atoi(field[2]);
    mask = MODE_SET;
    gpsd_report(3, "GPGSA sets mode %d\n", session->gpsdata.newdata.mode);
    session->gpsdata.pdop = atof(field[session->device_type->channels+3]);
    session->gpsdata.hdop = atof(field[session->device_type->channels+4]);
    session->gpsdata.vdop = atof(field[session->device_type->channels+5]);
    session->gpsdata.satellites_used = 0;
    memset(session->gpsdata.used,0,sizeof(session->gpsdata.used));
    for (i = 0; i < session->device_type->channels; i++) {
        int prn = atoi(field[i+3]);
        if (prn > 0)
	    session->gpsdata.used[session->gpsdata.satellites_used++] = prn;
    }
    mask |= HDOP_SET | VDOP_SET | PDOP_SET | USED_SET;

    return mask;
}

static gps_mask_t processGPGSV(int count, char *field[], struct gps_device_t *session)
/* GPS Satellites in View */
{
    /*
        GSV,2,1,08,01,40,083,46,02,17,308,41,12,07,344,39,14,22,228,45*75
           2            Number of sentences for full data
           1            sentence 1 of 2
           08           Total number of satellites in view
           01           Satellite PRN number
           40           Elevation, degrees
           083          Azimuth, degrees
           46           Signal-to-noise ratio in decibels
           <repeat for up to 4 satellites per sentence>
                There my be up to three GSV sentences in a data packet
     */
    int n, fldnum;
    if (count <= 3) {
	gpsd_zero_satellites(&session->gpsdata);
        return ERROR_SET;
    }
    session->driver.nmea.await = atoi(field[1]);
    if (sscanf(field[2], "%d", &session->driver.nmea.part) < 1) {
	gpsd_zero_satellites(&session->gpsdata);
        return ERROR_SET;
    } else if (session->driver.nmea.part == 1)
	gpsd_zero_satellites(&session->gpsdata);

    for (fldnum = 4; fldnum < count; ) {
	if (session->gpsdata.satellites >= session->device_type->channels) {
	    gpsd_report(0, "internal error - too many satellites!\n");
	    gpsd_zero_satellites(&session->gpsdata);
	    break;
	}
	session->gpsdata.PRN[session->gpsdata.satellites]       = atoi(field[fldnum++]);
	session->gpsdata.elevation[session->gpsdata.satellites] = atoi(field[fldnum++]);
	session->gpsdata.azimuth[session->gpsdata.satellites]   = atoi(field[fldnum++]);
	session->gpsdata.ss[session->gpsdata.satellites]        = atoi(field[fldnum++]);
	/*
	 * Incrementing this unconditionally falls afoul of chipsets like 
	 * the Motorola Oncore GT+ that emit empty fields at the end of the
	 * last sentence in a GPGSV set if the number of satellites is not
	 * a multiple of 4.
	 */
	if (session->gpsdata.PRN[session->gpsdata.satellites] != 0)
	    session->gpsdata.satellites++;
    }
    if (session->driver.nmea.part == session->driver.nmea.await && atoi(field[3]) != session->gpsdata.satellites)
	gpsd_report(0, "GPGSV field 3 value of %d != actual count %d\n",
		    atoi(field[3]), session->gpsdata.satellites);

    /* not valid data until we've seen a complete set of parts */
    if (session->driver.nmea.part < session->driver.nmea.await) {
	gpsd_report(3, "Partial satellite data (%d of %d).\n", session->driver.nmea.part, session->driver.nmea.await);
	return ERROR_SET;
    }
    /*
     * This sanity check catches an odd behavior of SiRF-II based GPSes.
     * When they can't see any satellites at all (like, inside a
     * building) they sometimes cough up a hairball in the form of a
     * GSV packet with all the azimuth entries 0 (but nonzero
     * elevations).  This behavior was observed under SiRF firmware
     * revision 231.000.000_A2.
     */
    for (n = 0; n < session->gpsdata.satellites; n++)
	if (session->gpsdata.azimuth[n] != 0)
	    goto sane;
    gpsd_report(3, "Satellite data no good.\n");
    gpsd_zero_satellites(&session->gpsdata);
    return ERROR_SET;
  sane:
    gpsd_report(3, "Satellite data OK.\n");
    return SATELLITE_SET;
    }

static gps_mask_t processPGRME(int c UNUSED, char *field[], struct gps_device_t *session)
/* Garmin Estimated Position Error */
{
    /*
       $PGRME,15.0,M,45.0,M,25.0,M*22
	1    = horizontal error estimate
        2    = units
	3    = vertical error estimate
        4    = units
	5    = spherical error estimate
        6    = units
     *
     * Garmin won't say, but the general belief is that these are 50% CEP.
     * We follow the advice at <http://gpsinformation.net/main/errors.htm>.
     */
    session->gpsdata.newdata.eph = atof(field[1]) * (GPSD_CONFIDENCE/CEP50_SIGMA);
    session->gpsdata.newdata.epv = atof(field[3]) * (GPSD_CONFIDENCE/CEP50_SIGMA);
    session->gpsdata.epe = atof(field[5]) * (GPSD_CONFIDENCE/CEP50_SIGMA);
    return HERR_SET | VERR_SET | PERR_SET;
}

static gps_mask_t processGPZDA(int c UNUSED, char *field[], struct gps_device_t *session)
/* Time & Date */
{
    gps_mask_t mask = TIME_SET;
    /*
      $GPZDA,160012.71,11,03,2004,-1,00*7D
      1) UTC time (hours, minutes, seconds, may have fractional subsecond)
      2) Day, 01 to 31
      3) Month, 01 to 12
      4) Year (4 digits)
      5) Local zone description, 00 to +- 13 hours
      6) Local zone minutes description, apply same sign as local hours
      7) Checksum
     */
    merge_hhmmss(field[1], session);
    session->driver.nmea.date.tm_year = atoi(field[4]) - 1900;
    session->driver.nmea.date.tm_mon = atoi(field[3])-1;
    session->driver.nmea.date.tm_mday = atoi(field[2]);
    session->gpsdata.newdata.time = (double)mkgmtime(&session->driver.nmea.date)+session->driver.nmea.subseconds;
    if (session->gpsdata.sentence_time != session->gpsdata.newdata.time)
	mask |= CYCLE_START_SET;
    session->gpsdata.sentence_time = session->gpsdata. newdata.time;
    return mask;
}

#ifdef TNT_ENABLE
static gps_mask_t processTNTHTM(int c UNUSED, char *field[], struct gps_device_t *session)
{
    /*
     * Proprietary sentence for True North Technologies Magnetic Compass.
     * This may also apply to some Honeywell units since they may have been
     * designed by True North.

        HTM,x.x,a,x.x,a,x.x,a,x.x,x.x*hh<cr><lf>
	Fields in order:
	1. True heading in degrees
	2. magnetometer status character:
		C = magnetometer calibration alarm
		L = low alarm
		M = low warning
		N = normal
		O = high warning
		P = high alarm
		V = magnetometer voltage level alarm
	3. pitch angle
	4. pitch status character - see field 2 above
	5. roll angle
	6. roll status character - see field 2 above
	7. dip angle
	8. relative magnitude horizontal component of earth's magnetic field
	*hh          mandatory nmea_checksum
     */
    gps_mask_t mask;
    mask = ONLINE_SET;

    //gpsd_zero_satellites(&session->gpsdata);

    session->gpsdata.fix.time = timestamp();
    session->gpsdata.newdata.track = atof(field[1]);
    session->gpsdata.headingStatus = *field[2];
    mask |= (TRACK_SET | MODE_SET);
    session->gpsdata.status = STATUS_FIX;	/* could be DGPS_FIX, we can't tell */
    //session->gpsdata.newdata.mode = MODE_2D;
    session->gpsdata.newdata.mode = MODE_2D;
    //session->gpsdata.newdata.mode = MODE_NO_FIX;

    session->gpsdata.newdata.pitch = atof(field[3]);
    session->gpsdata.pitchStatus = *field[4];

    session->gpsdata.newdata.roll = atof(field[5]);
    session->gpsdata.rollStatus = *field[6];

    session->gpsdata.newdata.dip = atof(field[7]);
    session->gpsdata.horzField = atof(field[8]);

    //gpsd_report(5, "Heading %lf  %c.\n", session->gpsdata.fix.track, session->gpsdata.fix.headingStatus);
    return mask;
}
#endif /* TNT_ENABLE */

#ifdef __UNUSED__
static short nmea_checksum(char *sentence, unsigned char *correct_sum)
/* is the checksum on the specified sentence good? */
{
    unsigned char sum = '\0';
    char c, *p = sentence, csum[3];

    while ((c = *p++) != '*' && c != '\0')
	sum ^= c;
    if (correct_sum)
        *correct_sum = sum;
    (void)snprintf(csum, sizeof(csum), "%02X", sum);
    return(toupper(csum[0])==toupper(p[0]))&&(toupper(csum[1])==toupper(p[1]));
}
#endif /* __ UNUSED__ */

/**************************************************************************
 *
 * Entry points begin here
 *
 **************************************************************************/

gps_mask_t nmea_parse(char *sentence, struct gps_device_t *session)
/* parse an NMEA sentence, unpack it into a session structure */
{
    typedef gps_mask_t (*nmea_decoder)(int count, char *f[], struct gps_device_t *session);
    static struct {
	char *name;
	nmea_decoder decoder;
    } nmea_phrase[] = {
	{"RMC", 	processGPRMC},
	{"GGA",         processGPGGA},
	{"GLL", 	processGPGLL},
	{"GSA", 	processGPGSA},
	{"GSV", 	processGPGSV},
	{"VTG", 	NULL},		/* ignore Velocity Track made Good */
	{"ZDA", 	processGPZDA},
	{"PGRMC",	NULL},		/* ignore Garmin Sensor Config */
	{"PGRME",	processPGRME},
	{"PGRMI",	NULL},		/* ignore Garmin Sensor Init */
	{"PGRMO",	NULL},		/* ignore Garmin Sentence Enable */
#ifdef TNT_ENABLE
	{"PTNTHTM",	processTNTHTM},
#endif /* TNT_ENABLE */
    };
    unsigned char buf[NMEA_MAX+1];

    int count;
    gps_mask_t retval = 0;
    unsigned int i;
    char *p, *field[NMEA_MAX], *s;
#ifdef __UNUSED__
    unsigned char sum;

    if (!nmea_checksum(sentence+1, &sum)) {
        gpsd_report(1, "Bad NMEA checksum: '%s' should be %02X\n",
                   sentence, sum);
        return 0;
    }
#endif /* __ UNUSED__ */

    /*@ -usedef @*//* splint 3.1.1 seems to have a bug here */
    /* make an editable copy of the sentence */
    strncpy((char *)buf, sentence, NMEA_MAX);
    /* discard the checksum part */
    for (p = (char *)buf; (*p != '*') && (*p >= ' '); ) ++p;
    *p = '\0';
    /* split sentence copy on commas, filling the field array */
    for (count = 0, p = (char *)buf; p != NULL && *p != '\0'; ++count, p = strchr(p, ',')) {
	*p = '\0';
	field[count] = ++p;
    }
    /* dispatch on field zero, the sentence tag */
    for (i = 0; i < (unsigned)(sizeof(nmea_phrase)/sizeof(nmea_phrase[0])); ++i) {
	s = field[0];
	if (strlen(nmea_phrase[i].name) == 3)
	    s += 2;	/* skip talker ID */
        if (strcmp(nmea_phrase[i].name, s) == 0) {
	    if (nmea_phrase[i].decoder) {
		retval = (nmea_phrase[i].decoder)(count, field, session);
		strncpy(session->gpsdata.tag, nmea_phrase[i].name, MAXTAGLEN);
		session->gpsdata.sentence_length = strlen(sentence);
	    } else
		retval = ONLINE_SET;		/* unknown sentence */
	    break;
	}
    }
    /*@ +usedef @*/
    return retval;
}
#endif /* NMEA_ENABLE */

void nmea_add_checksum(char *sentence)
/* add NMEA checksum to a possibly  *-terminated sentence */
{
    unsigned char sum = '\0';
    char c, *p = sentence;

    if (*p == '$') {
	p++;
    } else {
        gpsd_report(1, "Bad NMEA sentence: '%s'\n", sentence);
    }
    while ( ((c = *p) != '*') && (c != '\0')) {
	sum ^= c;
	p++;
    }
    *p++ = '*';
    (void)snprintf(p, 5, "%02X\r\n", (unsigned)sum);
}

int nmea_send(int fd, const char *fmt, ... )
/* ship a command to the GPS, adding * and correct checksum */
{
    int status;
    char buf[BUFSIZ];
    va_list ap;

    va_start(ap, fmt) ;
    (void)vsnprintf(buf, sizeof(buf)-5, fmt, ap);
    va_end(ap);
    if (fmt[0] == '$') {
	strcat(buf, "*");
	nmea_add_checksum(buf);
    } else
	strcat(buf, "\r\n");
    status = (int)write(fd, buf, strlen(buf));
    if (status == (int)strlen(buf)) {
	gpsd_report(2, "=> GPS: %s\n", buf);
	return status;
    } else {
	gpsd_report(2, "=> GPS: %s FAILED\n", buf);
	return -1;
    }
}
