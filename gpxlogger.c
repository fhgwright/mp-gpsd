#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include <math.h>
#include <time.h>
#include <signal.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <dbus/dbus-glib.h>

#include <glib/gprintf.h>

DBusConnection* connection;
FILE* gpxfile;

static char *author = "Amaury Jacquot";
static char *copyright = "GPL v 2.0";

static int intrack = 0;
static time_t tracklimit = 5; /* seconds */

static struct {
	time_t	old_time;
	time_t	time;
	gint32	mode;
	gdouble	ept;
	gdouble	latitude;
	gdouble	longitude;
	gdouble eph;
	gdouble altitude;
	gdouble epv;
	gdouble	track;
	gdouble epd;
	gdouble	speed;
	gdouble	eps;
	gdouble	climb;
	gdouble epc;
	//gdouble separation;
} gpsfix;


static void print_gpx_trk_start (void) {
	fprintf (gpxfile, " <trk>\n");
	fprintf (gpxfile, "  <trkseg>\n");
	fflush (gpxfile);
}

static void print_gpx_trk_end (void) {
	fprintf (gpxfile, "  </trkseg>\n");
	fprintf (gpxfile, " </trk>\n");
	fflush (gpxfile);
}

static DBusHandlerResult handle_gps_fix (DBusMessage* message) {
	DBusMessageIter	iter;
	double		temp_time;
	
	if (!dbus_message_iter_init (message, &iter)) {
		/* we have a problem */
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	/* this should be smarter :D */
	temp_time		= dbus_message_iter_get_double (&iter);
	gpsfix.time = floor(temp_time);
	dbus_message_iter_next (&iter);
	gpsfix.mode		= dbus_message_iter_get_int32 (&iter);
	dbus_message_iter_next (&iter);
	gpsfix.ept		= dbus_message_iter_get_double (&iter);
	dbus_message_iter_next (&iter);
	gpsfix.latitude		= dbus_message_iter_get_double (&iter);
	dbus_message_iter_next (&iter);
	gpsfix.longitude	= dbus_message_iter_get_double (&iter);
	dbus_message_iter_next (&iter);
	gpsfix.eph		= dbus_message_iter_get_double (&iter);
	dbus_message_iter_next (&iter);
	gpsfix.altitude		= dbus_message_iter_get_double (&iter);
	dbus_message_iter_next (&iter);
	gpsfix.epv		= dbus_message_iter_get_double (&iter);
	dbus_message_iter_next (&iter);
	gpsfix.track		= dbus_message_iter_get_double (&iter);
	dbus_message_iter_next (&iter);
	gpsfix.epd		= dbus_message_iter_get_double (&iter);
	dbus_message_iter_next (&iter);
	gpsfix.speed		= dbus_message_iter_get_double (&iter);
	dbus_message_iter_next (&iter);
	gpsfix.eps		= dbus_message_iter_get_double (&iter);
	dbus_message_iter_next (&iter);
	gpsfix.climb		= dbus_message_iter_get_double (&iter);
	dbus_message_iter_next (&iter);
	gpsfix.epc		= dbus_message_iter_get_double (&iter);
	dbus_message_iter_next (&iter);
	//gpsfix.separation	= dbus_message_iter_get_double (&iter);

	/* 
	 * we have a fix there - log the point
	 */
	if (gpxfile&&(gpsfix.time!=gpsfix.old_time)&&gpsfix.mode>1) {
		struct tm 	time;

		/* Make new track if the jump in time is above
		 * tracklimit.  Handle jumps both forward and
		 * backwards in time.  The clock sometimes jump
		 * backward when gpsd is submitting junk on the
		 * dbus. */
		if (fabs(gpsfix.time - gpsfix.old_time) > tracklimit) {
			print_gpx_trk_end();
			intrack = 0;
		}

		if (!intrack) {
			print_gpx_trk_start();
			intrack = 1;
		}
		
		gpsfix.old_time = gpsfix.time;
		fprintf (gpxfile, "   <trkpt lat=\"%f\" lon=\"%f\">\n", gpsfix.latitude, gpsfix.longitude);
		fprintf (gpxfile, "    <ele>%f</ele>\n", gpsfix.altitude);
		gmtime_r (&(gpsfix.time), &time);
		fprintf (gpxfile, "    <time>%04d-%02d-%02dT%02d:%02d:%02dZ</time>\n",
				time.tm_year+1900, time.tm_mon+1, time.tm_mday,
				time.tm_hour, time.tm_min, time.tm_sec);
		if (gpsfix.mode==1)
			fprintf (gpxfile, "    <fix>none</fix>\n");
		else
			fprintf (gpxfile, "    <fix>%dd</fix>\n", gpsfix.mode);
		fprintf (gpxfile, "   </trkpt>\n");
		fflush (gpxfile);
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

static void print_gpx_header (void) {
	if (!gpxfile) return;

	fprintf (gpxfile, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
	fprintf (gpxfile, "<gpx version=\"1.1\" creator=\"navsys logger\"\n");
	fprintf (gpxfile, "        xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n");
	fprintf (gpxfile, "        xmlns=\"http://www.topografix.com/GPX/1.1\"\n");
	fprintf (gpxfile, "        xsi:schemaLocation=\"http://www.topografix.com/GPS/1/1\n");
	fprintf (gpxfile, "        http://www.topografix.com/GPX/1/1/gpx.xsd\">\n");
	fprintf (gpxfile, " <metadata>\n");
	fprintf (gpxfile, "  <name>NavSys GPS logger dump</name>\n");
	fprintf (gpxfile, "  <author>%s</author>\n", author);
	fprintf (gpxfile, "  <copyright>%s</copyright>\n", copyright);
	fprintf (gpxfile, " </metadata>\n");
	fflush (gpxfile);
}

static void print_gpx_footer (void) {
	if (intrack)
		print_gpx_trk_end();
	fprintf (gpxfile, "</gpx>\n");
	fclose (gpxfile);
}

static void quit_handler (int signum) {
	syslog (LOG_INFO, "exiting, signal %d received", signum);
	print_gpx_footer ();
	exit (0);
}

/*
 * Message dispatching function
 *
 */
static DBusHandlerResult signal_handler (
		DBusConnection* connection, DBusMessage* message) {
	/* dummy, need to use the variable for some reason */
	connection = NULL;
	
	if (dbus_message_is_signal (message, "org.gpsd", "fix")) 
		return handle_gps_fix (message);
	/*
	 * ignore all other messages
	 */
	
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int main (int argc, char** argv) {
	GMainLoop* mainloop;
	DBusError error;

	/* initializes the gpsfix data structure */
	bzero (&gpsfix, sizeof(gpsfix));

	/* catch all interesting signals */
	signal (SIGTERM, quit_handler);
	signal (SIGQUIT, quit_handler);
	signal (SIGINT, quit_handler);
			
	
	openlog ("gpxlogger", LOG_PID | LOG_NDELAY , LOG_DAEMON);
	syslog (LOG_INFO, "---------- STARTED ----------");
	
	if (argc<2) {
		fprintf (stderr, "need the filename as an argument\n");
		return 1;
	}
	
	gpxfile = fopen (argv[1],"w");
	if (!gpxfile) {
		syslog (LOG_CRIT, "Unable to open destination file %s", argv[1]);
		return 2;
	}
	print_gpx_header ();
	
	mainloop = g_main_loop_new (NULL, FALSE);

	dbus_error_init (&error);
	connection = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
	if (dbus_error_is_set (&error)) {
		syslog (LOG_CRIT, "%s: %s", error.name, error.message);
		return 3;
	}
	
	dbus_bus_add_match (connection, "type='signal'", &error);
	if (dbus_error_is_set (&error)) {
		syslog (LOG_CRIT, "unable to add match for signals %s: %s", error.name, error.message);
		return 4;
	}

	if (!dbus_connection_add_filter (connection, (DBusHandleMessageFunction)signal_handler, NULL, NULL)) {
		syslog (LOG_CRIT, "unable to register filter with the connection");
		return 5;
	}
	
	dbus_connection_setup_with_g_main (connection, NULL);

	g_main_loop_run (mainloop);
	return 0;
}
