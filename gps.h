/* gps.h -- interface of the libgps library */

#ifndef gps_h
#  define gps_h 1

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <time.h>

#define MAXCHANNELS	12	/* maximum GPS channels (*not* satellites!) */

struct life_t {
/* lifetime structure to be associated with some piece of data */
    time_t	last_refresh;
    int		changed;
};
#define INIT(stamp, now)	stamp.last_refresh=now
#define REFRESH(stamp)	stamp.last_refresh = time(NULL)
#define SEEN(stamp) stamp.last_refresh

struct gps_data_t {
    int	online;			/* 1 if GPS is on line, 0 if not.
				 *
				 * Note: gpsd clears this flag when sentences
				 * fail to show up within the GPS's normal
				 * send cycle time. If the host-to-GPS 
				 * link is lossy enough to drop entire
				 * sentences, this flag will be
				 * prone to false negatives.
				 */
    struct life_t online_stamp;
    char utc[28];		/* UTC date/time as "yyy-mm-ddThh:mm:ss.sssZ".
				 *
				 * Updated on every valid fix (GGA, GLL or
				 * GPRMC). The hhmmss.ss part is reliable to
				 * within one GPS send cycle time (normally one
				 * second).  Altitude could be one send cycle
				 * older than the timestamp if the last
				 * sentence was GPRMC.
				 * 
				 * Within one GPS send cycle after midnight,
				 * if the last sentence was GGA or GLL and not
				 * GPRMC, the date could be off by one.
				 *
				 * The century part of the year is spliced in
				 * from host-machine time. 
				 */
    /*
     * Position/velocity fields are only valid when the last_refresh
     * field of the associated timestamp is nonzero, in which case it
     * tells when the data was collected.
     */
    double latitude;		/* Latitude in degrees */
    double longitude;		/* Longitude in degrees */
    struct life_t latlon_stamp;
    double altitude;		/* Altitude in meters */
    struct life_t altitude_stamp;

    /* velocity */
    double speed;		/* Speed over ground, knots */
    struct life_t speed_stamp;
    double track;		/* Course made good (relative to true north) */
    struct life_t track_stamp;

    /* status of fix */
    int    status;		/* Do we have a fix? */
#define STATUS_NO_FIX	0	/* no */
#define STATUS_FIX	1	/* yes, without DGPS */
#define STATUS_DGPS_FIX	2	/* yes, with DGPS */
    struct life_t status_stamp;
    int    mode;		/* Mode of fix */
#define MODE_NOT_SEEN	0	/* GSA not seen yet */
#define MODE_NO_FIX	1	/* none */
#define MODE_2D  	2	/* good for latitude/longitude */
#define MODE_3D  	3	/* good for altitude too */
    struct life_t mode_stamp;

    /* precision of fix */
    int satellites_used;	/* Number of satellites used in solution */
    int used[MAXCHANNELS];	/* Used in last fix? */
    double pdop, hdop, vdop;	/* Dilution of precision */
    struct life_t fix_quality_stamp;

    /* satellite status */
    int satellites;	/* # of satellites in view */
    int PRN[MAXCHANNELS];	/* PRNs of satellite */
    int elevation[MAXCHANNELS];	/* elevation of satellite */
    int azimuth[MAXCHANNELS];	/* azimuth */
    int ss[MAXCHANNELS];	/* signal strength */
    int part, await;		/* for tracking GSV parts */
    struct life_t satellite_stamp;

    /* these members are private */
    int gps_fd;			/* socket or file descriptor to GPS */
    void (*raw_hook)(char *buf);/* Raw-mode hook for GPS data. */
};

struct gps_data_t *gps_open(const char *host, const char *port);
int gps_close(struct gps_data_t *);
int gps_query(struct gps_data_t *gpsdata, const char *requests);
int gps_poll(struct gps_data_t *gpsdata);
void gps_set_raw_hook(struct gps_data_t *gpsdata, void (*hook)(char *buf));

/* some multipliers for interpreting GPS output */
#define METERS_TO_FEET	3.2808399	/* Imperial (U.S./British) feet */
#define METERS_TO_MILES	0.00062137119	/* International miles */
#define KNOTS_TO_MPH	1.1507794	/* International miles and knots */
#define KNOTS_TO_KPH	1.852		/* International knots */
#define PI 3.14159265358979323846	/* for radians-to-degrees and vv. */

/* gps_open() error return values */
#define NL_NOSERVICE	-1	/* can't get service entry */
#define NL_NOHOST	-2	/* can't get host entry */
#define NL_NOPROTO	-3	/* can't get protocol entry */
#define NL_NOSOCK	-4	/* can't create socket */
#define NL_NOSOCKOPT	-5	/* error SETSOCKOPT SO_REUSEADDR */
#define NL_NOCONNECT	-6	/* can't connect to host */

#define DEFAULT_GPSD_PORT	"2947"	/* IANA assignment */

#ifdef __cplusplus
}  /* End of the 'extern "C"' block */
#endif

#endif /* gps_h */
/* gps.h ends here */

