/*****************************************************************************

This is a decoder for RTCM-104, an obscure and complicated serial
protocol used for broadcasting pseudorange corrections from
differential-GPS reference stations.  The applicable
standard is

RTCM RECOMMENDED STANDARDS FOR DIFFERENTIAL NAVSTAR GPS SERVICE,
RTCM PAPER 194-93/SC 104-STD

Ordering instructions are accessible from <http://www.rtcm.org/>
under "Publications".

Also applicable is ITU-R M.823: "Technical characteristics of
differential transmissions for global navigation satellite systems
from maritime radio beacons in the frequency band 283.5 - 315 kHz in
region 1 and 285 - 325 kHz in regions 2 & 3."

The RTCM protocol uses as a transport layer the GPS satellite downlink
protocol described in IS-GPS-200, the Navstar GPS Interface
Specification.  This code relies on the lower-level packet-assembly
code for that protocol in isgps.c.

The lower layer's job is done when it has assembled a message of up to
33 words of clean parity-checked data.  At this point this upper layer
takes over.  struct rtcm_msg_t is overlaid on the buffer and the bitfields
are used to extract pieces of it.  Those pieces are copied and (where
necessary) reassembled into a struct rtcm_t.

This code and the contents of isgps.c are evolved from code by Wolgang
Rupprecht.  Wolfgang's decoder was loosely based on one written by
John Sager in 1999 (in particular the dump function emits a close
descendant of Sager's dump format).  Here are John Sager's original
notes:

The RTCM decoder prints a legible representation of the input data.
The RTCM SC-104 specification is copyrighted, so I cannot
quote it - in fact, I have never read it! Most of the information
used to develop the decoder came from publication ITU-R M.823.
This is a specification of the data transmitted from LF DGPS
beacons in the 300kHz band. M.823 contains most of those parts of
RTCM SC-104 directly relevant to the air interface (there
are one or two annoying and vital omissions!). Information
about the serial interface format was gleaned from studying
the output of a beacon receiver test program made available on
Starlink's website.

*****************************************************************************/

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h> 		/* for round() */

#include "gpsd.h"

/*
 * Structures for interpreting words in an RTCM-104 message (after
 * parity checking and removing inversion).
 *
 * The RTCM standard is less explicit than it should be about signed-integer
 * representations.  Two's compliment is specified for prc and rrc (msg1wX),
 * but not everywhere.
 */

#define	ZCOUNT_SCALE	0.6	/* sec */
#define	PCSMALL		0.02	/* meters */
#define	PCLARGE		0.32	/* meters */
#define	RRSMALL		0.002	/* meters/sec */
#define	RRLARGE		0.032	/* meters/sec */

#define MAXPCSMALL     (0x7FFF * PCSMALL)  /* 16-bits signed */
#define MAXRRSMALL     (0x7F   * RRSMALL)  /*  8-bits signed */

#define XYZ_SCALE	0.01	/* meters */
#define DXYZ_SCALE	0.1	/* meters */
#define	LA_SCALE	(90.0/32767.0)	/* degrees */
#define	LO_SCALE	(180.0/32767.0)	/* degrees */
#define	FREQ_SCALE	0.1	/* kHz */
#define	FREQ_OFFSET	190.0	/* kHz */
#define CNR_OFFSET	24	/* dB */
#define TU_SCALE	5	/* minutes */

#pragma pack(1)

#ifndef WORDS_BIGENDIAN	/* little-endian, like x86 */

struct rtcm_msg_t {
    struct rtcm_msghw1 {			/* header word 1 */
	uint            parity:6;
	uint            refstaid:10;	/* reference station ID */
	uint            msgtype:6;		/* RTCM message type */
	uint            preamble:8;		/* fixed at 01100110 */
	uint            _pad:2;
    } w1;

    struct rtcm_msghw2 {			/* header word 2 */
	uint            parity:6;
	uint            stathlth:3;		/* station health */
	uint            frmlen:5;
	uint            sqnum:3;
	uint            zcnt:13;
	uint            _pad:2;
    } w2;

    union {
	/* msg 1 - differential gps corrections */
	struct rtcm_msg1 {
	    struct b_correction_t {
		struct {			/* msg 1 word 3 */
		    uint            parity:6;
		    int             pc1:16;
		    uint            satident1:5;	/* satellite ID */
		    uint            udre1:2;
		    uint            scale1:1;
		    uint            _pad:2;
		} w3;

		struct {			/* msg 1 word 4 */
		    uint            parity:6;
		    uint            satident2:5;	/* satellite ID */
		    uint            udre2:2;
		    uint            scale2:1;
		    uint            issuedata1:8;
		    int             rangerate1:8;
		    uint            _pad:2;
		} w4;

		struct {			/* msg 1 word 5 */
		    uint            parity:6;
		    int             rangerate2:8;
		    int             pc2:16;
		    uint            _pad:2;
		} w5;

		struct {			/* msg 1 word 6 */
		    uint            parity:6;
		    int             pc3_h:8;
		    uint            satident3:5;	/* satellite ID */
		    uint            udre3:2;
		    uint            scale3:1;
		    uint            issuedata2:8;
		    uint            _pad:2;
		} w6;

		struct {			/* msg 1 word 7 */
		    uint            parity:6;
		    uint            issuedata3:8;
		    int             rangerate3:8;
		    uint            pc3_l:8;		/* NOTE: uint for low byte */
		    uint            _pad:2;
		} w7;
	    } corrections[(RTCM_WORDS_MAX - 2) / 5];
	} type1;

	/* msg 3 - reference station parameters */
	struct rtcm_msg3 {
	    struct {
		uint        parity:6;
		uint	    x_h:24;
		uint        _pad:2;
	    } w3;
	    struct {
		uint        parity:6;
		uint	    y_h:16;
		uint	    x_l:8;
		uint        _pad:2;
	    } w4;
	    struct {
		uint        parity:6;
		uint	    z_h:8;
		uint	    y_l:16;
		uint        _pad:2;
	    } w5;

	    struct {
		uint        parity:6;
		uint	    z_l:24;
		uint        _pad:2;
	    } w6;
	} type3;

	/* msg 4 - reference station datum */
	struct rtcm_msg4 {
	    struct {
		uint        parity:6;
		uint	    datum_alpha_char2:8;
		uint	    datum_alpha_char1:8;
		uint	    spare:4;
		uint	    dat:1;
		uint	    dgnss:3;
		uint        _pad:2;
	    } w3;
	    struct {
		uint        parity:6;
		uint	    datum_sub_div_char2:8;
		uint	    datum_sub_div_char1:8;
		uint	    datum_sub_div_char3:8;
		uint        _pad:2;
	    } w4;
	    struct {
		uint        parity:6;
		uint	    dy_h:8;
		uint	    dx:16;
		uint        _pad:2;
	    } w5;
	    struct {
		uint        parity:6;
		uint	    dz:24;
		uint	    dy_l:8;
		uint        _pad:2;
	    } w6;
	} type4;

	/* msg 5 - constellation health */
	struct rtcm_msg5 {
	    struct b_health_t {
		uint        parity:6;
		uint	    unassigned:2;
		uint	    time_unhealthy:4;
		uint	    loss_warn:1;
		uint	    new_nav_data:1;
		uint	    health_enable:1;
		uint	    cn0:5;
		uint	    data_health:3;
		uint	    issue_of_data_link:1;
		uint	    sat_id:5;
		uint	    reserved:1;
		uint        _pad:2;
	    } health[MAXHEALTH];
	} type5;

	/* msg 6 - null message */

	/* msg 7 - beacon almanac */
	struct rtcm_msg7 {
	    struct b_station_t {
		struct {
		    uint            parity:6;
		    int	    	    lon_h:8;
		    int	            lat:16;
		    uint            _pad:2;
		} w3;
		struct {
		    uint            parity:6;
		    uint	    freq_h:6;
		    uint	    range:10;
		    uint	    lon_l:8;
		    uint            _pad:2;
		} w4;
		struct {
		    uint            parity:6;
		    uint	    encoding:1;
		    uint	    sync_type:1;
		    uint	    mod_mode:1;
		    uint	    bit_rate:3;
		    /*
		     * ITU-R M.823-2 page 9 and RTCM-SC104 v2.1 pages
		     * 4-21 and 4-22 are in conflict over the next two
		     * field sizes.  ITU says 9+3, RTCM says 10+2.
		     * The latter correctly decodes the USCG station
		     * id's so I'll use that one here. -wsr
		     */
		    uint	    station_id:10;
		    uint	    health:2;
		    uint	    freq_l:6;
		    uint            _pad:2;
		} w5;
	    } almanac[(RTCM_WORDS_MAX - 2)/3];
	} type7;

	/* msg 16 - text msg */
	struct rtcm_msg16 {
	    struct {
		uint        parity:6;
		uint	    byte3:8;
		uint	    byte2:8;
		uint	    byte1:8;
		uint        _pad:2;
	    } txt[RTCM_WORDS_MAX-2];
	} type16;

	/* unknown message */
	isgps30bits_t	rtcm_msgunk[RTCM_WORDS_MAX-2];
    } msg_type;
};

#endif /* LITTLE_ENDIAN */

#if WORDS_BIGENDIAN
/* This struct was generated from the above using invert-bitfields.pl */
#ifndef S_SPLINT_S	/* splint thinks it's a duplicate definition */

struct rtcm_msg_t {
    struct rtcm_msghw1 {			/* header word 1 */
	uint            _pad:2;
	uint            preamble:8;		/* fixed at 01100110 */
	uint            msgtype:6;		/* RTCM message type */
	uint            refstaid:10;	/* reference station ID */
	uint            parity:6;
    } w1;

    struct rtcm_msghw2 {			/* header word 2 */
	uint            _pad:2;
	uint            zcnt:13;
	uint            sqnum:3;
	uint            frmlen:5;
	uint            stathlth:3;		/* station health */
	uint            parity:6;
    } w2;

    union {
	/* msg 1 - differential gps corrections */
	struct rtcm_msg1 {
	    struct b_correction_t {
		struct {			/* msg 1 word 3 */
		    uint            _pad:2;
		    uint            scale1:1;
		    uint            udre1:2;
		    uint            satident1:5;	/* satellite ID */
		    int             pc1:16;
		    uint            parity:6;
		} w3;

		struct {			/* msg 1 word 4 */
		    uint            _pad:2;
		    int             rangerate1:8;
		    uint            issuedata1:8;
		    uint            scale2:1;
		    uint            udre2:2;
		    uint            satident2:5;	/* satellite ID */
		    uint            parity:6;
		} w4;

		struct {			/* msg 1 word 5 */
		    uint            _pad:2;
		    int             pc2:16;
		    int             rangerate2:8;
		    uint            parity:6;
		} w5;

		struct {			/* msg 1 word 6 */
		    uint            _pad:2;
		    uint            issuedata2:8;
		    uint            scale3:1;
		    uint            udre3:2;
		    uint            satident3:5;	/* satellite ID */
		    int             pc3_h:8;
		    uint            parity:6;
		} w6;

		struct {			/* msg 1 word 7 */
		    uint            _pad:2;
		    uint            pc3_l:8;		/* NOTE: uint for low byte */
		    int             rangerate3:8;
		    uint            issuedata3:8;
		    uint            parity:6;
		} w7;
	    } corrections[(RTCM_WORDS_MAX - 2) / 5];
	} type1;

	/* msg 3 - reference station parameters */
	struct rtcm_msg3 {
	    struct {
		uint        _pad:2;
		uint	    x_h:24;
		uint        parity:6;
	    } w3;
	    struct {
		uint        _pad:2;
		uint	    x_l:8;
		uint	    y_h:16;
		uint        parity:6;
	    } w4;
	    struct {
		uint        _pad:2;
		uint	    y_l:16;
		uint	    z_h:8;
		uint        parity:6;
	    } w5;

	    struct {
		uint        _pad:2;
		uint	    z_l:24;
		uint        parity:6;
	    } w6;
	} type3;

	/* msg 4 - reference station datum */
	struct rtcm_msg4 {
	    struct {
		uint        _pad:2;
		uint	    dgnss:3;
		uint	    dat:1;
		uint	    spare:4;
		uint	    datum_alpha_char1:8;
		uint	    datum_alpha_char2:8;
		uint        parity:6;
	    } w3;
	    struct {
		uint        _pad:2;
		uint	    datum_sub_div_char3:8;
		uint	    datum_sub_div_char1:8;
		uint	    datum_sub_div_char2:8;
		uint        parity:6;
	    } w4;
	    struct {
		uint        _pad:2;
		uint	    dx:16;
		uint	    dy_h:8;
		uint        parity:6;
	    } w5;
	    struct {
		uint        _pad:2;
		uint	    dy_l:8;
		uint	    dz:24;
		uint        parity:6;
	    } w6;
	} type4;

	/* msg 5 - constellation health */
	struct rtcm_msg5 {
	    struct b_health_t {
		uint        _pad:2;
		uint	    reserved:1;
		uint	    sat_id:5;
		uint	    issue_of_data_link:1;
		uint	    data_health:3;
		uint	    cn0:5;
		uint	    health_enable:1;
		uint	    new_nav_data:1;
		uint	    loss_warn:1;
		uint	    time_unhealthy:4;
		uint	    unassigned:2;
		uint        parity:6;
	    } health[MAXHEALTH];
	} type5;

	/* msg 6 - null message */

	/* msg 7 - beacon almanac */
	struct rtcm_msg7 {
	    struct b_station_t {
		struct {
		    uint            _pad:2;
		    int	            lat:16;
		    int	    	    lon_h:8;
		    uint            parity:6;
		} w3;
		struct {
		    uint            _pad:2;
		    uint	    lon_l:8;
		    uint	    range:10;
		    uint	    freq_h:6;
		    uint            parity:6;
		} w4;
		struct {
		    uint            _pad:2;
		    uint	    freq_l:6;
		    uint	    health:2;
		    uint	    station_id:10;
			     /* see comments in LE struct above. */
		    uint	    bit_rate:3;
		    uint	    mod_mode:1;
		    uint	    sync_type:1;
		    uint	    encoding:1;
		    uint            parity:6;
		} w5;
	    } almanac[(RTCM_WORDS_MAX - 2)/3];
	} type7;

	/* msg 16 - text msg */
	struct rtcm_msg16 {
	    struct {
		uint        _pad:2;
		uint	    byte1:8;
		uint	    byte2:8;
		uint	    byte3:8;
		uint        parity:6;
	    } txt[RTCM_WORDS_MAX-2];
	} type16;

	/* unknown message */
	isgps30bits_t	rtcm_msgunk[RTCM_WORDS_MAX-2];
    } msg_type;
};

#endif /* S_SPLINT_S */
#endif /* BIG ENDIAN */

static unsigned int tx_speed[] = { 25, 50, 100, 110, 150, 200, 250, 300 };

void rtcm_unpack(struct gps_device_t *session)
/* break out the raw bits into the content fields */
{
    int len;
    unsigned int n, w;
    struct rtcm_t *tp = &session->gpsdata.rtcm;
    struct rtcm_msg_t *msg = (struct rtcm_msg_t *)session->driver.isgps.buf;

    tp->type = msg->w1.msgtype;
    tp->length = msg->w2.frmlen;
    tp->zcount = msg->w2.zcnt * ZCOUNT_SCALE;
    tp->refstaid = msg->w1.refstaid;
    tp->seqnum = msg->w2.sqnum;
    tp->stathlth = msg->w2.stathlth;

    len = (int)tp->length;
    n = 0;
    switch (tp->type) {
    case 1:
    case 9:
	{
	    struct b_correction_t    *m = &msg->msg_type.type1.corrections[0];

	    while (len >= 0) {
		if (len >= 2) {
		    tp->msg_data.ranges.sat[n].ident      = m->w3.satident1;
		    tp->msg_data.ranges.sat[n].udre       = m->w3.udre1;
		    tp->msg_data.ranges.sat[n].issuedata  = m->w4.issuedata1;
		    tp->msg_data.ranges.sat[n].rangerr    = m->w3.pc1 * 
			(m->w3.scale1 ? PCLARGE : PCSMALL);
		    tp->msg_data.ranges.sat[n].rangerate  = m->w4.rangerate1 * 
					(m->w3.scale1 ? RRLARGE : RRSMALL);
		    n++;
		}
		if (len >= 4) {
		    tp->msg_data.ranges.sat[n].ident      = m->w4.satident2;
		    tp->msg_data.ranges.sat[n].udre       = m->w4.udre2;
		    tp->msg_data.ranges.sat[n].issuedata  = m->w6.issuedata2;
		    tp->msg_data.ranges.sat[n].rangerr    = m->w5.pc2 * 
			(m->w4.scale2 ? PCLARGE : PCSMALL);
		    tp->msg_data.ranges.sat[n].rangerate  = m->w5.rangerate2 * 
			(m->w4.scale2 ? RRLARGE : RRSMALL);
		    n++;
		}
		if (len >= 5) {
		    tp->msg_data.ranges.sat[n].ident       = m->w6.satident3;
		    tp->msg_data.ranges.sat[n].udre        = m->w6.udre3;
		    tp->msg_data.ranges.sat[n].issuedata   = m->w7.issuedata3;
		    /*@ -shiftimplementation @*/
		    tp->msg_data.ranges.sat[n].rangerr     = ((m->w6.pc3_h<<8)|(m->w7.pc3_l)) *
					(m->w6.scale3 ? PCLARGE : PCSMALL);
		    tp->msg_data.ranges.sat[n].rangerate   = m->w7.rangerate3 * 
					(m->w6.scale3 ? RRLARGE : RRSMALL);
		    /*@ +shiftimplementation @*/
		    n++;
		}
		len -= 5;
		m++;
	    }
	    tp->msg_data.ranges.nentries = n;
	}
	break;
    case 3:
        {
	    struct rtcm_msg3    *m = &msg->msg_type.type3;

	    if ((tp->msg_data.ecef.valid = len >= 4)) {
		tp->msg_data.ecef.x = ((m->w3.x_h<<8)|(m->w4.x_l))*XYZ_SCALE;
		tp->msg_data.ecef.y = ((m->w4.y_h<<16)|(m->w5.y_l))*XYZ_SCALE;
		tp->msg_data.ecef.z = ((m->w5.z_h<<24)|(m->w6.z_l))*XYZ_SCALE;
	    }
	}
	break;
    case 4:
	if ((tp->msg_data.reference.valid = len >= 2)){
	    struct rtcm_msg4    *m = &msg->msg_type.type4;

	    tp->msg_data.reference.system =
		    (m->w3.dgnss==0) ? gps :
			    ((m->w3.dgnss==1) ? glonass : unknown);
	    tp->msg_data.reference.sense = (m->w3.dat != 0) ? global : local;
	    if (m->w3.datum_alpha_char1){
		tp->msg_data.reference.datum[n++] = (char)(m->w3.datum_alpha_char1);
	    }
	    if (m->w3.datum_alpha_char2){
		tp->msg_data.reference.datum[n++] = (char)(m->w3.datum_alpha_char2);
	    }
	    if (m->w4.datum_sub_div_char1){
		tp->msg_data.reference.datum[n++] = (char)(m->w4.datum_sub_div_char1);
	    }
	    if (m->w4.datum_sub_div_char2){
		tp->msg_data.reference.datum[n++] = (char)(m->w4.datum_sub_div_char2);
	    }
	    if (m->w4.datum_sub_div_char3){
		tp->msg_data.reference.datum[n++] = (char)(m->w4.datum_sub_div_char3);
	    }
	    tp->msg_data.reference.datum[n++] = '\0';
	    if (len >= 4) {
		tp->msg_data.reference.dx = m->w5.dx * DXYZ_SCALE;
		tp->msg_data.reference.dy = ((m->w5.dy_h << 8) | m->w6.dy_l) * DXYZ_SCALE;
		tp->msg_data.reference.dz = m->w6.dz * DXYZ_SCALE;
	    } else 
		tp->msg_data.reference.sense = invalid;
	}
	break;
    case 5:
	for (n = 0; n < (unsigned)len; n++) {
	    struct consat_t *csp = &tp->msg_data.conhealth.sat[n];
	    struct b_health_t *m = &msg->msg_type.type5.health[n];

	    csp->ident = m->sat_id;
	    csp->iodl = m->issue_of_data_link!=0;
	    csp->health = m->data_health;
	    /*@i@*/csp->snr = (m->cn0?(m->cn0+CNR_OFFSET):SNR_BAD);
	    csp->health_en = m->health_enable;
	    csp->new_data = m->new_nav_data!=0;
	    csp->los_warning = m->loss_warn!=0;
	    csp->tou = m->time_unhealthy*TU_SCALE;
	}
	tp->msg_data.conhealth.nentries = n;
	break;
    case 7:
	for (w = 0; w < (unsigned)len; w++) {
	    struct station_t *np = &tp->msg_data.almanac.station[n];
	    struct b_station_t *mp = &msg->msg_type.type7.almanac[w];

	    np->latitude = mp->w3.lat * LA_SCALE;
	    /*@i@*/np->longitude = ((mp->w3.lon_h << 8) | mp->w4.lon_l) * LO_SCALE;
	    np->range = mp->w4.range;
	    np->frequency = (((mp->w4.freq_h << 6) | mp->w5.freq_l) * FREQ_SCALE) + FREQ_OFFSET;
	    np->health = mp->w5.health;
	    np->station_id = mp->w5.station_id,
	    np->bitrate = tx_speed[mp->w5.bit_rate];
	    n++;
	}
	tp->msg_data.almanac.nentries = (unsigned)(len/3);
	break;
    case 16:
	/*@ -boolops @*/
	for (w = 0; w < (unsigned)len; w++){
	    if (!msg->msg_type.type16.txt[w].byte1) {
		break;
	    }
	    tp->msg_data.message[n++] = (char)(msg->msg_type.type16.txt[w].byte1);
	    if (!msg->msg_type.type16.txt[w].byte2) {
		break;
	    }
	    tp->msg_data.message[n++] = (char)(msg->msg_type.type16.txt[w].byte2);
	    if (!msg->msg_type.type16.txt[w].byte3) {
		break;
	    }
	    tp->msg_data.message[n++] = (char)(msg->msg_type.type16.txt[w].byte3);
	}
	/*@ +boolops @*/
	tp->msg_data.message[n++] = '\0';
	break;

    default:
	memcpy(tp->msg_data.words, msg->msg_type.rtcm_msgunk, (RTCM_WORDS_MAX-2)*sizeof(isgps30bits_t));
	break;
    }
}

bool rtcm_repack(struct gps_device_t *session)
/* repack the content fields into the raw bits */
{
    int len, sval;
    unsigned int n, w, uval;
    struct rtcm_t *tp = &session->gpsdata.rtcm;
    struct rtcm_msg_t  *msg = (struct rtcm_msg_t *)session->driver.isgps.buf;
    struct rtcm_msghw1 *wp  = (struct rtcm_msghw1 *)session->driver.isgps.buf;

    memset(session->driver.isgps.buf, 0, sizeof(session->driver.isgps.buf));
    msg->w1.msgtype = tp->type;
    msg->w2.frmlen = tp->length;
    msg->w2.zcnt = (unsigned) round(tp->zcount / ZCOUNT_SCALE);
    msg->w1.refstaid = tp->refstaid;
    msg->w2.sqnum = tp->seqnum;
    msg->w2.stathlth = tp->stathlth;

    len = (int)tp->length;
    n = 0;
    switch (tp->type) {
    case 1:	/* S */
    case 9:
	{
	    struct b_correction_t    *m = &msg->msg_type.type1.corrections[0];

	    while (len >= 0) {
		if (len >= 2) {
		    struct rangesat_t *ssp = &tp->msg_data.ranges.sat[n];
		    m->w3.satident1 = ssp->ident;
		    m->w3.udre1 = ssp->udre;
		    m->w4.issuedata1 = ssp->issuedata;
		    m->w3.scale1 = (unsigned)((ssp->rangerr > MAXPCSMALL) ||
					      (ssp->rangerr < (-MAXPCSMALL)) ||
					      (ssp->rangerate > MAXRRSMALL) ||
					      (ssp->rangerate < (-MAXRRSMALL)));
		    m->w3.pc1 = (int) round(ssp->rangerr / (m->w3.scale1 ? PCLARGE : PCSMALL));
		    m->w4.rangerate1 = (int) round(ssp->rangerate / (m->w3.scale1 ? RRLARGE : RRSMALL));
		    n++;
		}
		if (len >= 4) {
		    struct rangesat_t *ssp = &tp->msg_data.ranges.sat[n];
		    m->w4.satident2 = ssp->ident;
		    m->w4.udre2 = ssp->udre;
		    m->w6.issuedata2 = ssp->issuedata;
		    m->w4.scale2 = (unsigned)((ssp->rangerr > MAXPCSMALL) ||
					      (ssp->rangerr < (-MAXPCSMALL)) ||
					      (ssp->rangerate > MAXRRSMALL) ||
					      (ssp->rangerate < (-MAXRRSMALL)));
		    m->w5.pc2 = (int) round(ssp->rangerr / (m->w4.scale2 ? PCLARGE : PCSMALL));
		    m->w5.rangerate2 = (int) round(ssp->rangerate / (m->w4.scale2 ? RRLARGE : RRSMALL));
		    n++;
		}
		if (len >= 5) {
		    struct rangesat_t *ssp = &tp->msg_data.ranges.sat[n];
		    m->w6.satident3 = ssp->ident;
		    m->w6.udre3 = ssp->udre;
		    m->w7.issuedata3 = ssp->issuedata;
		    m->w6.scale3 = (unsigned)((ssp->rangerr > MAXPCSMALL) ||
					      (ssp->rangerr < (-MAXPCSMALL)) ||
					      (ssp->rangerate > MAXRRSMALL) ||
					      (ssp->rangerate < (-MAXRRSMALL)));
		    sval = (int) round(ssp->rangerr / (m->w6.scale3 ? PCLARGE : PCSMALL));
		    /*@ -shiftimplementation @*/
		    m->w6.pc3_h = sval >> 8;
		    /*@ +shiftimplementation @*/
		    m->w7.pc3_l = (unsigned)sval & 0xff;
		    m->w7.rangerate3 = (int) round(ssp->rangerate / (m->w6.scale3 ? RRLARGE : RRSMALL));
		    n++;
		}
		len -= 5;
		m++;
	    }
	    tp->msg_data.ranges.nentries = n;
	}
	break;
    case 3:	/* R */
	if (tp->msg_data.ecef.valid) {
	    struct rtcm_msg3    *m = &msg->msg_type.type3;
	    unsigned x = (unsigned) round(tp->msg_data.ecef.x / XYZ_SCALE);
	    unsigned y = (unsigned) round(tp->msg_data.ecef.y / XYZ_SCALE);
	    unsigned z = (unsigned) round(tp->msg_data.ecef.z / XYZ_SCALE);

	    m->w4.x_l = x & 0xff;
	    m->w3.x_h = x >> 8;
	    m->w5.y_l = y & 0xffff;
	    m->w4.y_h = y >> 16;
	    m->w6.z_l = z & 0xffffff;
	    m->w5.z_h = z >> 24;
	}
	break;
    case 4:	/* D */
	if (tp->msg_data.reference.valid) {
	    struct rtcm_msg4    *m = &msg->msg_type.type4;

	    m->w3.dgnss = tp->msg_data.reference.system;
	    m->w3.dat = (unsigned)(tp->msg_data.reference.sense == global);
	    /*@ -predboolothers -type @*/
	    if (tp->msg_data.reference.datum[0])
		m->w3.datum_alpha_char1 = tp->msg_data.reference.datum[0];
	    else
		m->w3.datum_alpha_char1 = 0;
	    if (tp->msg_data.reference.datum[1])
		m->w3.datum_alpha_char2 = tp->msg_data.reference.datum[1];
	    else
		m->w3.datum_alpha_char2 = 0;
	    if (tp->msg_data.reference.datum[2])
		m->w4.datum_sub_div_char1 = tp->msg_data.reference.datum[2];
	    else
		m->w4.datum_sub_div_char1 = 0;
	    if (tp->msg_data.reference.datum[3])
		m->w4.datum_sub_div_char2 = tp->msg_data.reference.datum[3];
	    else
		m->w4.datum_sub_div_char2 = 0;
	    if (tp->msg_data.reference.datum[4])
		m->w4.datum_sub_div_char3 = tp->msg_data.reference.datum[4];
	    else
		m->w4.datum_sub_div_char3 = 0;
	    /*@ +predboolothers +type @*/
	    if (tp->msg_data.reference.system != unknown) {
		m->w5.dx = (uint)round(tp->msg_data.reference.dx / DXYZ_SCALE);
		uval = (uint)round(tp->msg_data.reference.dy / DXYZ_SCALE);
		m->w5.dy_h = uval >> 8;
		m->w6.dy_l = uval & 0xff;
		m->w6.dz = (uint)round(tp->msg_data.reference.dz / DXYZ_SCALE);
	    }
	}
	break;
    case 5:	/* C */
	for (n = 0; n < (unsigned)len; n++) {
	    struct consat_t *csp = &tp->msg_data.conhealth.sat[n];
	    struct b_health_t *m = &msg->msg_type.type5.health[n];

	    m->sat_id = csp->ident;
	    m->issue_of_data_link = (unsigned)csp->iodl;
	    m->data_health = csp->health;
	    m->cn0 = (csp->snr == SNR_BAD) ? 0 : (unsigned)csp->snr-CNR_OFFSET;
	    m->health_enable = csp->health_en;
	    m->new_nav_data = (unsigned)csp->new_data;
	    m->loss_warn = (unsigned)csp->los_warning;
	    m->time_unhealthy = (unsigned)(csp->tou / TU_SCALE);
	}
	break;
    case 7:	/* A */
	for (w = 0; w < (RTCM_WORDS_MAX - 2)/ 3; w++) {
	    struct station_t *np = &tp->msg_data.almanac.station[n++];
	    struct b_station_t *mp = &msg->msg_type.type7.almanac[w];

	    mp->w3.lat = (int) round(np->latitude / LA_SCALE);
	    sval = (int) round(np->longitude / LO_SCALE);
	    /*@ -shiftimplementation @*/
	    mp->w3.lon_h = sval >> 8;
	    /*@ +shiftimplementation @*/
	    mp->w4.lon_l = (unsigned)sval & 0xff;
	    mp->w4.range = np->range;
	    uval = (unsigned) round(((np->frequency-FREQ_OFFSET) / FREQ_SCALE));
	    mp->w4.freq_h = uval >> 6;
	    mp->w5.freq_l = uval & 0x3f;
	    mp->w5.health = np->health;
	    mp->w5.station_id = np->station_id;
	    mp->w5.bit_rate = 0;
	    for (uval = 0; uval < (unsigned)(sizeof(tx_speed)/sizeof(tx_speed[0])); uval++)
		if (tx_speed[uval] == np->bitrate) {
		    mp->w5.bit_rate = uval;
		    break;
		}
	    if (mp->w5.bit_rate == 0)
		return false;
	}
	tp->msg_data.almanac.nentries = n;
	break;
    case 16:	/* T */
	/*@ -boolops @*/
	for (w = 0; w < RTCM_WORDS_MAX - 2; w++){
	    if (!tp->msg_data.message[n]) {
		break;
	    }
	    msg->msg_type.type16.txt[w].byte1 = (unsigned)tp->msg_data.message[n++];
	    if (!tp->msg_data.message[n]) {
		break;
	    }
	    msg->msg_type.type16.txt[w].byte2 = (unsigned)tp->msg_data.message[n++];
	    if (!tp->msg_data.message[n]) {
		break;
	    }
	    msg->msg_type.type16.txt[w].byte3 = (unsigned)tp->msg_data.message[n++];
	}
	msg->w2.frmlen = w+1;
	/*@ +boolops @*/
	break;

    default:	/* U */
	memcpy(msg->msg_type.rtcm_msgunk, tp->msg_data.words, (RTCM_WORDS_MAX-2)*sizeof(isgps30bits_t));
	break;
    }

    /* compute parity for each word in the message */
    for (w = 0; w < tp->length; w++)
	wp[w].parity = isgps_parity(session->driver.isgps.buf[w]);

    /* FIXME: must do inversion here */
    return true;
}

static bool preamble_match(isgps30bits_t *w)
{
    return (((struct rtcm_msghw1 *)w)->preamble == PREAMBLE_PATTERN);
}

static bool length_check(struct gps_device_t *session)
{
    return session->driver.isgps.bufindex >= 2 
	&& session->driver.isgps.bufindex >= ((struct rtcm_msg_t *)session->driver.isgps.buf)->w2.frmlen + 2u;
}

enum isgpsstat_t rtcm_decode(struct gps_device_t *session, unsigned int c)
{
    enum isgpsstat_t res = isgps_decode(session, 
					preamble_match, 
					length_check, 
					RTCM_WORDS_MAX, 
					c);
    if (res == ISGPS_MESSAGE)
	rtcm_unpack(session);

    return res;
}

void rtcm_dump(struct gps_device_t *session, /*@out@*/char buf[], size_t buflen)
/* dump the contents of a parsed RTCM104 message */
{
    unsigned int n;

    (void)snprintf(buf, buflen, "H\t%u\t%u\t%0.1f\t%u\t%u\t%u\n",
	   session->gpsdata.rtcm.type,
	   session->gpsdata.rtcm.refstaid,
	   session->gpsdata.rtcm.zcount,
	   session->gpsdata.rtcm.seqnum,
	   session->gpsdata.rtcm.length,
	   session->gpsdata.rtcm.stathlth);

    switch (session->gpsdata.rtcm.type) {
    case 1:
    case 9:
	for (n = 0; n < session->gpsdata.rtcm.msg_data.ranges.nentries; n++) {
	    struct rangesat_t *rsp = &session->gpsdata.rtcm.msg_data.ranges.sat[n];
	    (void)snprintf(buf + strlen(buf), buflen - strlen(buf),
			   "S\t%u\t%u\t%u\t%0.1f\t%0.3f\t%0.3f\n",
			   rsp->ident,
			   rsp->udre,
			   rsp->issuedata,
			   session->gpsdata.rtcm.zcount,
			   rsp->rangerr,
			   rsp->rangerate);
	}
	break;

    case 3:
	if (session->gpsdata.rtcm.msg_data.ecef.valid)
	    (void)snprintf(buf + strlen(buf), buflen - strlen(buf),
			   "R\t%.2f\t%.2f\t%.2f\n",
			   session->gpsdata.rtcm.msg_data.ecef.x, 
			   session->gpsdata.rtcm.msg_data.ecef.y,
			   session->gpsdata.rtcm.msg_data.ecef.z);
	break;

    case 4:
	if (session->gpsdata.rtcm.msg_data.reference.valid)
	    (void)snprintf(buf + strlen(buf), buflen - strlen(buf),
			   "D\t%s\t%1d\t%s\t%.1f\t%.1f\t%.1f\n",
			   (session->gpsdata.rtcm.msg_data.reference.system==gps) ? "GPS"
			   : ((session->gpsdata.rtcm.msg_data.reference.system==glonass) ? "GLONASS"
			      : "UNKNOWN"),
			   session->gpsdata.rtcm.msg_data.reference.sense,
			   session->gpsdata.rtcm.msg_data.reference.datum,
			   session->gpsdata.rtcm.msg_data.reference.dx,
			   session->gpsdata.rtcm.msg_data.reference.dy,
			   session->gpsdata.rtcm.msg_data.reference.dz);
	break;

    case 5:
	for (n = 0; n < session->gpsdata.rtcm.msg_data.conhealth.nentries; n++) {
	    struct consat_t *csp = &session->gpsdata.rtcm.msg_data.conhealth.sat[n];
	    (void)snprintf(buf + strlen(buf), buflen - strlen(buf),
			   "C\t%2u\t%1u\t%1u\t%2d\t%1u\t%1u\t%1u\t%2u\n",
			   csp->ident,
			   (unsigned)csp->iodl,
			   (unsigned)csp->health,
			   csp->snr,
			   (unsigned)csp->health_en,
			   (unsigned)csp->new_data,
			   (unsigned)csp->los_warning,
			   csp->tou);
	}
	break;

    case 6: 			/* NOP msg */
	strcat(buf, "N\n");
	break;

    case 7:
	for (n = 0; n < session->gpsdata.rtcm.msg_data.almanac.nentries; n++) {
	    struct station_t *ssp = &session->gpsdata.rtcm.msg_data.almanac.station[n];
	    (void)snprintf(buf + strlen(buf), buflen - strlen(buf),
			   "A\t%.4f\t%.4f\t%u\t%.1f\t%u\t%u\t%u\n",
			   ssp->latitude,
			   ssp->longitude,
			   ssp->range,
			   ssp->frequency,
			   ssp->health,
			   ssp->station_id,
			   ssp->bitrate);
	}
	break;
    case 16:
	(void)snprintf(buf + strlen(buf), buflen - strlen(buf),
		       "T\t\"%s\"\n", session->gpsdata.rtcm.msg_data.message);
	break;

    default:
	for (n = 0; n < session->gpsdata.rtcm.length; n++)
	    (void)snprintf(buf + strlen(buf), buflen - strlen(buf),
			   "U\t0x%08x\n", session->gpsdata.rtcm.msg_data.words[n]);
	break;
    }

}

int rtcm_undump(/*@out@*/struct rtcm_t *rtcmp, char *buf)
/* merge a line of data into an RTCM structure, return 0 if done */
{
    int fldcount, v;
    unsigned n;
    char buf2[BUFSIZ];

    /*@ -usedef @*/
    switch (rtcmp->type) {
    case 0:
	fldcount = sscanf(buf, "H\t%u\t%u\t%lf\t%u\t%u\t%u\n",
			&rtcmp->type,
			&rtcmp->refstaid,
			&rtcmp->zcount,
			&rtcmp->seqnum,
			&rtcmp->length,
			&rtcmp->stathlth);
	if (fldcount != 6)
	    return -1;
	else
	    return 1;
	//break;

    case 1:
    case 9:
	{
	    struct rangesat_t *rsp = &rtcmp->msg_data.ranges.sat[rtcmp->msg_data.ranges.nentries++];
	    /* we ignore the third (zcount) field, it's in the parent */
	    fldcount = sscanf(buf,
			      "S\t%u\t%u\t%u\t%*f\t%lf\t%lf\n",
			      &rsp->ident,
			      &rsp->udre,
			      &rsp->issuedata,
			      &rsp->rangerr,
			      &rsp->rangerate);
	    if (fldcount != 5 || (rtcmp->type != 1 && rtcmp->type != 9))
		return (int)(-rtcmp->type-1);
	    else if (rtcmp->msg_data.ranges.nentries != rtcmp->length*3/5)
		return (int)(rtcmp->type+1);
	    else
		return 0;
	}
	//break;

    case 3:
	fldcount = sscanf(buf,
			  "R\t%lf\t%lf\t%lf\n",
			  &rtcmp->msg_data.ecef.x, 
			  &rtcmp->msg_data.ecef.y,
			  &rtcmp->msg_data.ecef.z);
	if (fldcount != 3 || rtcmp->type != 3)
	    return -4;
	else {
	    rtcmp->msg_data.ecef.valid = true;
	    return 0;
	}
	//break;

    case 4:
	fldcount = sscanf(buf,
			   "D\t%s\t%1d\t%s\t%lf\t%lf\t%lf\n",
			  buf2,
			  &v,
			  (char *)&rtcmp->msg_data.reference.datum,
			  &rtcmp->msg_data.reference.dx,
			  &rtcmp->msg_data.reference.dy,
			  &rtcmp->msg_data.reference.dz);
	if (fldcount != 6 || rtcmp->type != 4)
	    return -5;
	else {
	    if (strcmp(buf2, "GPS") == 0)
		rtcmp->msg_data.reference.system = gps;
	    else if (strcmp(buf2, "GLONASS") == 0)
		rtcmp->msg_data.reference.system = glonass;
	    else
		rtcmp->msg_data.reference.system = unknown;
	    rtcmp->msg_data.reference.sense = (v == 1) ? global : ((v == 0) ? local : invalid);
	    rtcmp->msg_data.reference.valid = true;
	    return 0;
	}
	//break;

    case 5:
	{
	    struct consat_t *csp = &rtcmp->msg_data.conhealth.sat[rtcmp->msg_data.conhealth.nentries++];

	    fldcount = sscanf(buf,
			      "C\t%2u\t%1u\t%1u\t%2d\t%1u\t%1u\t%1u\t%2u\n",
			      &csp->ident,
			      (unsigned int *)&csp->iodl,
			      &csp->health,
			      &csp->snr,
			      &csp->health_en,
			      (unsigned int *)&csp->new_data,
			      (unsigned int *)&csp->los_warning,
			      &csp->tou);
	    if (fldcount != 8 || rtcmp->type != 5)
		return -6;
	    else if (rtcmp->msg_data.conhealth.nentries < rtcmp->length)
		return 6;
	    else
		return 0;
	}
	//break;

    case 6: 			/* NOP msg */
	if (buf[0] != 'N')
	    return -7;
	else
	    return 0;
	//break;

    case 7:
	{
	    struct station_t *ssp = &rtcmp->msg_data.almanac.station[rtcmp->msg_data.almanac.nentries++];
	    fldcount = sscanf(buf,
			      "A\t%lf\t%lf\t%u\t%lf\t%u\t%u\t%u\n",
			      &ssp->latitude,
			      &ssp->longitude,
			      &ssp->range,
			      &ssp->frequency,
			      &ssp->health,
			      &ssp->station_id,
			      &ssp->bitrate);
	    if (fldcount != 7 || rtcmp->type != 7)
		return 8;
	    else if (rtcmp->msg_data.almanac.nentries < rtcmp->length/3)
		return 8;
	    else
		return 0;
	}
	//break;

    case 16:
	fldcount = sscanf(buf, "T\t\"%[^\"]\"\n", rtcmp->msg_data.message);
	if (fldcount != 1)
	    return 16;
	else
	    return 0;
	//break;

    default:
	for (n = 0; n < (unsigned)(sizeof(rtcmp->msg_data.words)/sizeof(rtcmp->msg_data.words[0])); n++)
	    if (rtcmp->msg_data.words[n] == 0)
		break;
	fldcount = sscanf(buf, "U\t0x%08x\n", &v);
	if (fldcount != 1)
	    return (int)(-rtcmp->type-1);
	else {
	    rtcmp->msg_data.words[n] = (isgps30bits_t)v;
	    if (n == rtcmp->length-1)
		return 0;
	    else
		return (int)(rtcmp->type+1);
	}
	//break;
    }
    /*@ +usedef @*/
}

#ifdef __UNUSED__
/*
 * The RTCM words are 30-bit words.  We will lay them into memory into
 * 30-bit (low-end justified) chunks.  To write them out we will write
 * 5 Magnavox-format bytes where the low 6-bits of the byte are 6-bits
 * of the 30-word msg.
 */
void rtcm_output_mag(isgps30bits_t * ip)
/* ship an RTCM message to standard output in Magnavox format */
{
    static isgps30bits_t w = 0;
    int             len;
    static uint     sqnum = 0;

    len = ((struct rtcm_msg *) ip)->w2.frmlen + 2;
    ((struct rtcm_msg *) ip)->w2.sqnum = sqnum++;
    sqnum &= 0x7;

    while (len-- > 0) {
	w <<= 30;
	w |= *ip++ & W_DATA_MASK;

	w |= rtcmparity(w);

	/* weird-assed inversion */
	if (w & P_30_MASK)
	    w ^= W_DATA_MASK;

	/* msb first */
	putchar(MAG_TAG_DATA | reverse_bits[(w >> 24) & 0x3f]);
	putchar(MAG_TAG_DATA | reverse_bits[(w >> 18) & 0x3f]);
	putchar(MAG_TAG_DATA | reverse_bits[(w >> 12) & 0x3f]);
	putchar(MAG_TAG_DATA | reverse_bits[(w >> 6) & 0x3f]);
	putchar(MAG_TAG_DATA | reverse_bits[(w) & 0x3f]);
    }
}
#endif /* UNUSED */
