/* SPDX-License-Identifier: MIT */
/*
 * kissbufd - ACKMODE-based transmit buffer management for KISS TNCs
 * without hardware flow control (such as the NinoTNC).
 *
 * Host program (kissattach, ...)  <-- pty -->  kissbufd  <-- serial -->  TNC
 *
 * Every data frame is sent to the TNC as an ACKMODE frame with a unique
 * internal ID. The TNC returns the ID once the frame has left over the air,
 * which frees its share of the TNC buffer. Frames are only released to the
 * TNC while the raw KISS bytes in flight stay within FILL% of the buffer.
 */
#include "compat.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "kiss.h"
#include "serial.h"
#include "tracker.h"
#include "txq.h"

#ifndef KB_VERSION
# define KB_VERSION "0.1.0"
#endif

#define BUF_MIN	    64u
#define BUF_MAX	    ( 1024u * 1024u)
#define HOST_OBUF   ( 64u * 1024u)
#define MAX_DECODED ( BUF_MAX + 16u)
#define RD_CHUNK    4096

struct obuf {
	uint8_t *b;
	size_t len;
	size_t cap;
};

struct stats {
	uint64_t host_frames;
	uint64_t tnc_frames_tx; /* data frames sent to TNC */
	uint64_t tnc_cmds_tx;	/* non-data commands passed through */
	uint64_t tnc_bytes_tx;
	uint64_t acks;
	uint64_t acks_relayed;
	uint64_t acks_unknown;
	uint64_t ack_timeouts;
	uint64_t lost_disconnect;
	uint64_t giants;
	uint64_t malformed;
	uint64_t queue_drops;
	uint64_t host_rx_frames; /* TNC -> host frames */
	uint64_t host_drops;
	uint64_t reconnects;
	uint32_t peak_inflight;
};

static struct {
	struct serial_cfg ser;
	int sfd;
	uint64_t reconnect_at;
	int open_fail_logged;

	int ptm;
	int pts;
	char ptsname[PATH_MAX];
	const char *link;
	const char *pidfile;

	struct kiss_decoder hdec;
	struct kiss_decoder sdec;
	struct obuf sout;
	struct obuf hout;
	struct tracker trk;
	struct txq q;

	uint32_t bufsize;
	uint32_t fill_pct;
	uint32_t limit;
	uint64_t timeout_ms;
	uint64_t reconnect_ms;
	size_t qlimit;
	int verbose;
	int use_syslog;
	int daemonize;

	struct stats st;
} S;

static volatile sig_atomic_t g_quit;
static volatile sig_atomic_t g_stats;

/* ------------------------------------------------------------------ */
/* logging                                                            */

#if defined( __GNUC__) || defined( __clang__)
# define KB_PRINTF( f, a) __attribute__( ( format( printf, f, a)))
#else
# define KB_PRINTF( f, a)
#endif

static void kb_log( int prio, const char *fmt, ...) KB_PRINTF( 2, 3);

static void kb_log( int prio, const char *fmt, ...) {
	va_list ap;

	if ( prio == LOG_DEBUG && S.verbose < 2)
		return;
	if ( prio == LOG_INFO && S.verbose < 1)
		return;
	va_start( ap, fmt);
	if ( S.use_syslog)
		vsyslog( prio, fmt, ap);
	else {
		const char *tag;

		switch ( prio) {
		case LOG_ERR: tag = "error"; break;
		case LOG_WARNING: tag = "warning"; break;
		case LOG_NOTICE: tag = "notice"; break;
		case LOG_INFO: tag = "info"; break;
		default: tag = "debug"; break;
		}
		fprintf( stderr, "kissbufd: %s: ", tag);
		vfprintf( stderr, fmt, ap);
		fputc( '\n', stderr);
	}
	va_end( ap);
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */

static uint64_t mono_ms( void) {
	struct timespec ts;

	clock_gettime( CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)( ts.tv_nsec / 1000000);
}

static int set_nonblock( int fd) {
	int fl = fcntl( fd, F_GETFL);

	if ( fl < 0)
		return -1;
	return fcntl( fd, F_SETFL, fl | O_NONBLOCK);
}

static int obuf_init( struct obuf *o, size_t cap) {
	o->b = malloc( cap);
	o->len = 0;
	o->cap = cap;
	return o->b == NULL ? -1 : 0;
}

static size_t obuf_room( const struct obuf *o) {
	return o->cap - o->len;
}

/* Returns 0 (all or partially written), -1 on hard error. */
static int obuf_flush( int fd, struct obuf *o) {
	while ( o->len > 0) {
		ssize_t n = write( fd, o->b, o->len);

		if ( n < 0) {
			if ( errno == EINTR)
				continue;
			if ( errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			return -1;
		}
		if ( n == 0)
			return 0;
		if ( (size_t)n < o->len)
			memmove( o->b, o->b + n, o->len - (size_t)n);
		o->len -= (size_t)n;
	}
	return 0;
}

static int parse_u32( const char *s, uint32_t lo, uint32_t hi, uint32_t *out) {
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul( s, &end, 10);
	if ( errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

/* ------------------------------------------------------------------ */
/* host side (pty)                                                    */

static void host_send( const uint8_t *hdr, size_t hlen, const uint8_t *data, size_t dlen) {
	size_t need = kiss_encoded_size( hdr, hlen, data, dlen);

	if ( need > obuf_room( &S.hout)) {
		/* Nobody is reading the pty (or it is far behind). */
		S.st.host_drops++;
		kb_log( LOG_DEBUG, "pty output full, dropped %zu byte frame", need);
		return;
	}
	S.hout.len += kiss_encode( S.hout.b + S.hout.len, obuf_room( &S.hout), hdr, hlen, data, dlen);
	(void)obuf_flush( S.ptm, &S.hout);
}

/* ------------------------------------------------------------------ */
/* TNC side                                                           */

static int serial_flush( void);
static void serial_lost( const char *why);

/*
 * Move frames from the host queue to the TNC while they fit.
 * Strict FIFO: a frame that does not fit blocks those behind it, so
 * ordering (including of non-data commands) is preserved.
 */
static void pump_tx( void) {
	struct txq_item *it;
	uint64_t now = mono_ms();

	while ( S.sfd >= 0 && ( it = txq_peek( &S.q)) != NULL) {
		if ( it->kind == TXQ_RAW) {
			size_t need = kiss_encoded_size( &it->type, 1, it->data, it->len);

			if ( need > obuf_room( &S.sout))
				break;
			S.sout.len += kiss_encode( S.sout.b + S.sout.len, obuf_room( &S.sout), &it->type, 1, it->data, it->len);
			S.st.tnc_cmds_tx++;
			S.st.tnc_bytes_tx += need;
			kb_log( LOG_DEBUG, "cmd 0x%02x (%zu bytes) passed through", it->type, it->len);
			txq_pop( &S.q);
			continue;
		} else {
			uint8_t hdr[3];
			uint16_t id;
			size_t cost;
			struct tx_rec *r;

			if ( tracker_peek_id( &S.trk, &id) != 0)
				break; /* all IDs in flight: wait for ACKs */
			hdr[0] = KISS_TYPE( KISS_PORT( it->type), KISS_CMD_ACKMODE);
			net16_put( hdr + 1, id);
			cost = kiss_encoded_size( hdr, 3, it->data, it->len);
			if ( !tracker_fits( &S.trk, (uint32_t)cost))
				break; /* backpressure: wait for ACKs */
			if ( cost > obuf_room( &S.sout))
				break;
			r = tracker_commit( &S.trk, id, (uint32_t)cost, KISS_PORT( it->type), it->kind == TXQ_ACKMODE, it->host_id, now);
			if ( r == NULL) {
				kb_log( LOG_ERR, "out of memory tracking frame");
				break;
			}
			S.sout.len += kiss_encode( S.sout.b + S.sout.len, obuf_room( &S.sout), hdr, 3, it->data, it->len);
			S.st.tnc_frames_tx++;
			S.st.tnc_bytes_tx += cost;
			if ( S.trk.bytes > S.st.peak_inflight)
				S.st.peak_inflight = S.trk.bytes;
			if ( it->kind == TXQ_ACKMODE)
				kb_log( LOG_DEBUG, "tx id 0x%04x (host 0x%04x) %zu bytes, in flight %u/%u", id, it->host_id, cost, S.trk.bytes, S.trk.limit);
			else
				kb_log( LOG_DEBUG, "tx id 0x%04x %zu bytes, in flight %u/%u", id, cost, S.trk.bytes, S.trk.limit);
			txq_pop( &S.q);
		}
	}
	if ( S.sfd >= 0 && S.sout.len > 0)
		(void)serial_flush();
}

static int serial_flush( void) {
	if ( obuf_flush( S.sfd, &S.sout) != 0) {
		serial_lost( strerror( errno));
		return -1;
	}
	return 0;
}

static void serial_lost( const char *why) {
	uint64_t now = mono_ms();

	kb_log( LOG_WARNING, "TNC link lost (%s): %u frame(s) / %u bytes on the TNC considered lost; holding %u queued frame(s)", why, S.trk.count, S.trk.bytes, S.q.count);
	S.st.lost_disconnect += S.trk.count;
	tracker_clear( &S.trk);
	S.sout.len = 0;
	kiss_decoder_reset( &S.sdec);
	if ( S.sfd >= 0)
		close( S.sfd);
	S.sfd = -1;
	S.open_fail_logged = 0;
	S.reconnect_at = now + S.reconnect_ms;
}

static void serial_try_open( uint64_t now) {
	int fd = serial_open( &S.ser);

	if ( fd < 0) {
		if ( !S.open_fail_logged) {
			kb_log( LOG_WARNING, "cannot open %s: %s (retrying every %llu ms)", S.ser.path, strerror( errno), (unsigned long long)S.reconnect_ms);
			S.open_fail_logged = 1;
		}
		S.reconnect_at = now + S.reconnect_ms;
		return;
	}
	S.sfd = fd;
	S.open_fail_logged = 0;
	S.st.reconnects++;
	kiss_decoder_reset( &S.sdec);
	kb_log( LOG_NOTICE, "TNC link up on %s (%ld %d%c%d), %u frame(s) queued", S.ser.path, S.ser.baud, S.ser.databits, S.ser.parity, S.ser.stopbits, S.q.count);
	pump_tx();
}

static void handle_tnc_frame( const uint8_t *buf, size_t len) {
	uint8_t type = buf[0];

	if ( type != KISS_CMD_RETURN && KISS_CMD( type) == KISS_CMD_ACKMODE) {
		struct tx_rec *r;
		uint16_t id;

		if ( len < 3) {
			S.st.malformed++;
			kb_log( LOG_DEBUG, "short ACK from TNC discarded");
			return;
		}
		id = net16_get( buf + 1);
		r = tracker_find( &S.trk, id);
		if ( r == NULL) {
			S.st.acks_unknown++;
			kb_log( LOG_INFO, "ACK for unknown id 0x%04x discarded", id);
			return;
		}
		S.st.acks++;
		if ( r->host_ack) {
			uint8_t hdr[3];

			hdr[0] = KISS_TYPE( r->port, KISS_CMD_ACKMODE);
			net16_put( hdr + 1, r->host_id);
			host_send( hdr, 3, NULL, 0);
			S.st.acks_relayed++;
			kb_log( LOG_DEBUG, "ack id 0x%04x -> host 0x%04x", id, r->host_id);
		} else
			kb_log( LOG_DEBUG, "ack id 0x%04x (internal)", id);
		tracker_release( &S.trk, r);
		pump_tx();
		return;
	}

	/* Received packet or anything else: relay to host unchanged. */
	S.st.host_rx_frames++;
	host_send( buf, 1, buf + 1, len - 1);
}

static void check_timeouts( uint64_t now) {
	struct tx_rec *r;
	int freed = 0;

	while ( ( r = S.trk.head) != NULL && now >= r->sent_ms + S.timeout_ms) {
		if ( r->host_ack)
			kb_log( LOG_WARNING, "MINOR ALARM: no ACK for id 0x%04x (host id 0x%04x, %u bytes) after %llu s, considered lost", r->wire_id, r->host_id, r->cost, (unsigned long long)( S.timeout_ms / 1000u));
		else
			kb_log( LOG_WARNING, "MINOR ALARM: no ACK for id 0x%04x (%u bytes) after %llu s, considered lost", r->wire_id, r->cost, (unsigned long long)( S.timeout_ms / 1000u));
		S.st.ack_timeouts++;
		tracker_release( &S.trk, r);
		freed = 1;
	}
	if ( freed)
		pump_tx();
}

/* ------------------------------------------------------------------ */
/* host frame intake                                                  */

static void handle_host_frame( const uint8_t *buf, size_t len) {
	uint8_t type = buf[0];
	enum txq_kind kind;
	uint16_t host_id = 0;
	const uint8_t *payload;
	size_t plen;
	int r;

	S.st.host_frames++;
	if ( type != KISS_CMD_RETURN && KISS_CMD( type) == KISS_CMD_DATA) {
		kind = TXQ_DATA;
		payload = buf + 1;
		plen = len - 1;
	} else if ( type != KISS_CMD_RETURN && KISS_CMD( type) == KISS_CMD_ACKMODE) {
		if ( len < 3) {
			S.st.malformed++;
			kb_log( LOG_WARNING, "ACKMODE frame from host without id, dropped");
			return;
		}
		kind = TXQ_ACKMODE;
		host_id = net16_get( buf + 1);
		payload = buf + 3;
		plen = len - 3;
	} else {
		kind = TXQ_RAW;
		payload = buf + 1;
		plen = len - 1;
	}

	if ( kind != TXQ_RAW) {
		/* Wire IDs never need escaping, so the cost is exact. */
		static const uint8_t probe[3] = { KISS_CMD_ACKMODE, 0, 0 };
		size_t cost = kiss_encoded_size( probe, 3, payload, plen);

		if ( cost > S.limit) {
			S.st.giants++;
			kb_log( LOG_WARNING, "giant frame (%zu KISS bytes > %u) dropped", cost, S.limit);
			return;
		}
	}

	r = txq_push( &S.q, kind, type, host_id, payload, plen);
	if ( r != 0) {
		S.st.queue_drops++;
		kb_log( LOG_WARNING, "transmit queue %s, frame dropped (%u frames / %zu bytes queued)", r == -1 ? "full" : "out of memory", S.q.count, S.q.bytes);
	}
}

/* ------------------------------------------------------------------ */
/* pty                                                                */

static int pty_open( void) {
	struct termios tio;
	const char *name;

	S.ptm = posix_openpt( O_RDWR | O_NOCTTY);
	if ( S.ptm < 0) {
		kb_log( LOG_ERR, "posix_openpt: %s", strerror( errno));
		return -1;
	}
	if ( grantpt( S.ptm) != 0 || unlockpt( S.ptm) != 0) {
		kb_log( LOG_ERR, "grantpt/unlockpt: %s", strerror( errno));
		return -1;
	}
	name = ptsname( S.ptm);
	if ( name == NULL || strlen( name) >= sizeof( S.ptsname)) {
		kb_log( LOG_ERR, "ptsname failed");
		return -1;
	}
	strcpy( S.ptsname, name);

	/*
	 * Keep the slave open ourselves: the master then never sees a hangup
	 * when the host program closes and reopens it, and we can put the
	 * line in raw mode before anyone attaches.
	 */
	S.pts = open( S.ptsname, O_RDWR | O_NOCTTY);
	if ( S.pts < 0) {
		kb_log( LOG_ERR, "open %s: %s", S.ptsname, strerror( errno));
		return -1;
	}
	if ( tcgetattr( S.pts, &tio) == 0) {
		cfmakeraw( &tio);
		(void)tcsetattr( S.pts, TCSANOW, &tio);
	}
	if ( set_nonblock( S.ptm) != 0) {
		kb_log( LOG_ERR, "fcntl: %s", strerror( errno));
		return -1;
	}
	return 0;
}

/* mkdir -p, mode 0755. Modifies nothing on success if dir exists. */
static int mkdir_p( char *dir) {
	char *p;

	for ( p = dir + 1; *p != '\0'; p++) {
		if ( *p != '/')
			continue;
		*p = '\0';
		if ( mkdir( dir, 0755) != 0 && errno != EEXIST) {
			*p = '/';
			return -1;
		}
		*p = '/';
	}
	if ( mkdir( dir, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int link_create( void) {
	struct stat sb;

	if ( S.link == NULL)
		return 0;
	if ( lstat( S.link, &sb) == 0) {
		if ( !S_ISLNK( sb.st_mode)) {
			kb_log( LOG_ERR, "%s exists and is not a symlink", S.link);
			return -1;
		}
		if ( unlink( S.link) != 0) {
			kb_log( LOG_ERR, "unlink %s: %s", S.link, strerror( errno));
			return -1;
		}
	}
	if ( symlink( S.ptsname, S.link) == 0)
		return 0;
	if ( errno == ENOENT) {
		/* Create the link's parent directories and retry. */
		char dir[PATH_MAX];
		char *p;

		if ( strlen( S.link) < sizeof( dir)) {
			strcpy( dir, S.link);
			p = strrchr( dir, '/');
			if ( p != NULL && p != dir) {
				*p = '\0';
				if ( mkdir_p( dir) == 0 && symlink( S.ptsname, S.link) == 0)
					return 0;
			} else
				errno = ENOENT;
		}
	}
	kb_log( LOG_ERR, "symlink %s -> %s: %s", S.link, S.ptsname, strerror( errno));
	return -1;
}

static void link_remove( void) {
	char buf[PATH_MAX];
	ssize_t n;

	if ( S.link == NULL)
		return;
	n = readlink( S.link, buf, sizeof( buf) - 1);
	if ( n < 0)
		return;
	buf[n] = '\0';
	if ( strcmp( buf, S.ptsname) == 0)
		(void)unlink( S.link);
}

/* ------------------------------------------------------------------ */
/* misc                                                               */

static void on_signal( int sig) {
	if ( sig == SIGUSR1)
		g_stats = 1;
	else
		g_quit = 1;
}

static void dump_stats( void) {
	int saved = S.verbose;

	S.verbose = 1;
	kb_log( LOG_INFO, "stats: host frames %llu, tx data %llu, tx cmds %llu, tx bytes %llu, acks %llu (relayed %llu, unknown %llu), timeouts %llu, lost on disconnect %llu, giants %llu, malformed %llu, queue drops %llu, rx->host %llu, pty drops %llu, link-ups %llu, peak in flight %u/%u, now in flight %u frames/%u bytes, queued %u", (unsigned long long)S.st.host_frames, (unsigned long long)S.st.tnc_frames_tx, (unsigned long long)S.st.tnc_cmds_tx, (unsigned long long)S.st.tnc_bytes_tx, (unsigned long long)S.st.acks, (unsigned long long)S.st.acks_relayed, (unsigned long long)S.st.acks_unknown, (unsigned long long)S.st.ack_timeouts, (unsigned long long)S.st.lost_disconnect, (unsigned long long)S.st.giants, (unsigned long long)S.st.malformed, (unsigned long long)S.st.queue_drops, (unsigned long long)S.st.host_rx_frames, (unsigned long long)S.st.host_drops, (unsigned long long)S.st.reconnects, S.st.peak_inflight, S.limit, S.trk.count, S.trk.bytes, S.q.count);
	S.verbose = saved;
}

static void write_pidfile( void) {
	FILE *f;

	if ( S.pidfile == NULL)
		return;
	f = fopen( S.pidfile, "w");
	if ( f == NULL) {
		kb_log( LOG_WARNING, "pidfile %s: %s", S.pidfile, strerror( errno));
		return;
	}
	fprintf( f, "%ld\n", (long)getpid());
	fclose( f);
}

static void usage( FILE *f) {
	/* clang-format off */
	fprintf( f,
"usage: kissbufd [-c config] [-s device] [-B bytes] [options]\n"
"\n"
"  -c file    read settings from file (command line options override it)\n"
"  -n         check the configuration and exit\n"
"  -s dev     TNC serial device (e.g. /dev/ttyACM0)       [device]\n"
"  -B bytes   TNC transmit buffer, raw KISS bytes (%u..%u) [buffer_size]\n"
"  -b baud    serial speed (default 57600)                [baud]\n"
"  -m mode    serial framing <5-8><N|E|O><1|2> (8N1)      [serial_mode]\n"
"  -R         drop DTR/RTS on open (default: assert)      [dtr = no]\n"
"  -F pct     fill the TNC buffer up to pct%% (95)         [fill_percent]\n"
"  -p path    symlink to the pty slave                    [pty_link]\n"
"  -t secs    ACK timeout (60)                            [ack_timeout]\n"
"  -r secs    serial reopen interval (2)                  [reopen_interval]\n"
"  -Q bytes   host-side queue limit (262144)              [queue_limit]\n"
"  -D         run as a daemon (implies syslog)            [daemon = yes]\n"
"  -f         stay in the foreground                      [daemon = no]\n"
"  -S         log to syslog instead of stderr             [syslog = yes]\n"
"  -P file    write pid file                              [pidfile]\n"
"  -v         verbose, repeat for debug                   [verbose = 0..2]\n"
"  -V         print version and exit\n"
"  -h         this help\n"
"\n"
"Config file keys are shown in brackets. In the foreground the pty slave\n"
"name is printed on stdout. SIGUSR1 logs statistics.\n",
	    BUF_MIN, BUF_MAX);
	/* clang-format on */
}

/* ------------------------------------------------------------------ */
/* settings (shared by the config file and the command line)          */

static int parse_bool( const char *s, int *out) {
	if ( !strcmp( s, "yes") || !strcmp( s, "true") || !strcmp( s, "on") || !strcmp( s, "1")) {
		*out = 1;
		return 0;
	}
	if ( !strcmp( s, "no") || !strcmp( s, "false") || !strcmp( s, "off") || !strcmp( s, "0")) {
		*out = 0;
		return 0;
	}
	return -1;
}

static char *xstrdup( const char *s) {
	char *d = strdup( s);

	if ( d == NULL) {
		fprintf( stderr, "kissbufd: out of memory\n");
		exit( 1);
	}
	return d;
}

static int setting_error( const char *where, const char *key, const char *what) {
	fprintf( stderr, "kissbufd: %s: %s: %s\n", where, key, what);
	return -1;
}

static int apply_setting( const char *key, const char *val, const char *where, void *arg) {
	uint32_t v;
	int b;

	(void)arg;
	if ( *val == '\0' && strcmp( key, "pty_link") && strcmp( key, "pidfile"))
		return setting_error( where, key, "empty value");

	if ( !strcmp( key, "device")) {
		S.ser.path = xstrdup( val);
	} else if ( !strcmp( key, "buffer_size")) {
		if ( parse_u32( val, BUF_MIN, BUF_MAX, &S.bufsize))
			return setting_error( where, key, "must be 64..1048576");
	} else if ( !strcmp( key, "baud")) {
		if ( parse_u32( val, 1, 100000000u, &v) || !serial_baud_supported( (long)v))
			return setting_error( where, key, "unsupported baud rate");
		S.ser.baud = (long)v;
	} else if ( !strcmp( key, "serial_mode")) {
		if ( serial_parse_mode( val, &S.ser) != 0)
			return setting_error( where, key, "want e.g. 8N1, 7E1");
	} else if ( !strcmp( key, "dtr")) {
		if ( parse_bool( val, &b))
			return setting_error( where, key, "want yes or no");
		S.ser.dtr = b;
	} else if ( !strcmp( key, "fill_percent")) {
		if ( parse_u32( val, 1, 100, &S.fill_pct))
			return setting_error( where, key, "must be 1..100");
	} else if ( !strcmp( key, "pty_link")) {
		S.link = *val ? xstrdup( val) : NULL;
	} else if ( !strcmp( key, "ack_timeout")) {
		if ( parse_u32( val, 1, 86400, &v))
			return setting_error( where, key, "must be 1..86400 s");
		S.timeout_ms = (uint64_t)v * 1000u;
	} else if ( !strcmp( key, "reopen_interval")) {
		if ( parse_u32( val, 1, 3600, &v))
			return setting_error( where, key, "must be 1..3600 s");
		S.reconnect_ms = (uint64_t)v * 1000u;
	} else if ( !strcmp( key, "queue_limit")) {
		if ( parse_u32( val, 1024, 64u * 1024u * 1024u, &v))
			return setting_error( where, key, "must be 1024..67108864");
		S.qlimit = v;
	} else if ( !strcmp( key, "daemon")) {
		if ( parse_bool( val, &S.daemonize))
			return setting_error( where, key, "want yes or no");
	} else if ( !strcmp( key, "syslog")) {
		if ( parse_bool( val, &S.use_syslog))
			return setting_error( where, key, "want yes or no");
	} else if ( !strcmp( key, "pidfile")) {
		S.pidfile = *val ? xstrdup( val) : NULL;
	} else if ( !strcmp( key, "verbose")) {
		if ( parse_u32( val, 0, 9, &v))
			return setting_error( where, key, "must be 0..2");
		S.verbose = (int)v;
	} else
		return setting_error( where, key, "unknown setting");
	return 0;
}

struct cli_override {
	const char *key;
	const char *val;
	char where[4];
};

int main( int argc, char **argv) {
	struct sigaction sa;
	struct cli_override *ov;
	uint8_t rbuf[RD_CHUNK];
	const char *config = NULL;
	char vbuf[4];
	int ch, oi, nov = 0, nverbose = 0, check_only = 0;

	memset( &S, 0, sizeof( S));
	serial_cfg_defaults( &S.ser);
	S.sfd = -1;
	S.ptm = S.pts = -1;
	S.fill_pct = 95;
	S.timeout_ms = 60u * 1000u;
	S.reconnect_ms = 2u * 1000u;
	S.qlimit = 256u * 1024u;

	ov = calloc( (size_t)argc + 1, sizeof( *ov));
	if ( ov == NULL)
		return 1;

	/* Collect command line settings; they are applied after the file. */
	while ( ( ch = getopt( argc, argv, "c:ns:B:b:m:RF:p:t:r:Q:DfSP:vVh")) != -1) {
		const char *key = NULL, *val = optarg;

		switch ( ch) {
		case 'c': config = optarg; continue;
		case 'n': check_only = 1; continue;
		case 's': key = "device"; break;
		case 'B': key = "buffer_size"; break;
		case 'b': key = "baud"; break;
		case 'm': key = "serial_mode"; break;
		case 'R':
			key = "dtr";
			val = "no";
			break;
		case 'F': key = "fill_percent"; break;
		case 'p': key = "pty_link"; break;
		case 't': key = "ack_timeout"; break;
		case 'r': key = "reopen_interval"; break;
		case 'Q': key = "queue_limit"; break;
		case 'D':
			key = "daemon";
			val = "yes";
			break;
		case 'f':
			key = "daemon";
			val = "no";
			break;
		case 'S':
			key = "syslog";
			val = "yes";
			break;
		case 'P': key = "pidfile"; break;
		case 'v': nverbose++; continue;
		case 'V': printf( "kissbufd %s\n", KB_VERSION); return 0;
		case 'h': usage( stdout); return 0;
		default: usage( stderr); return 2;
		}
		ov[nov].key = key;
		ov[nov].val = val;
		snprintf( ov[nov].where, sizeof( ov[nov].where), "-%c", ch);
		nov++;
	}
	if ( optind != argc) {
		free( ov);
		usage( stderr);
		return 2;
	}

	if ( config != NULL && conf_parse_file( config, apply_setting, NULL) != 0) {
		free( ov);
		return 2;
	}
	for ( oi = 0; oi < nov; oi++) {
		if ( apply_setting( ov[oi].key, ov[oi].val, ov[oi].where, NULL) != 0) {
			free( ov);
			return 2;
		}
	}
	free( ov);
	if ( nverbose > 0) {
		snprintf( vbuf, sizeof( vbuf), "%d", nverbose > 9 ? 9 : nverbose);
		(void)apply_setting( "verbose", vbuf, "-v", NULL);
	}
	if ( S.daemonize)
		S.use_syslog = 1;

	if ( S.ser.path == NULL || S.bufsize == 0) {
		fprintf( stderr, "kissbufd: a device (-s) and a buffer size (-B) are required\n");
		usage( stderr);
		return 2;
	}

	S.limit = (uint32_t)( (uint64_t)S.bufsize * S.fill_pct / 100u);
	if ( S.limit < 16) {
		fprintf( stderr, "kissbufd: effective buffer too small\n");
		return 2;
	}
	if ( check_only) {
		printf( "configuration OK: device %s %ld %d%c%d, buffer %u, fill limit %u bytes, pty link %s\n", S.ser.path, S.ser.baud, S.ser.databits, S.ser.parity, S.ser.stopbits, S.bufsize, S.limit, S.link ? S.link : "(none)");
		return 0;
	}

	if ( S.use_syslog)
		openlog( "kissbufd", LOG_PID, LOG_DAEMON);

	tracker_init( &S.trk, S.limit, (uint16_t)( mono_ms() & 0xFFFFu));
	txq_init( &S.q, S.qlimit);
	if ( kiss_decoder_init( &S.hdec, MAX_DECODED) != 0 || kiss_decoder_init( &S.sdec, MAX_DECODED) != 0 || obuf_init( &S.sout, (size_t)S.limit + 64u * 1024u) != 0 || obuf_init( &S.hout, HOST_OBUF) != 0) {
		kb_log( LOG_ERR, "out of memory");
		return 1;
	}

	if ( pty_open() != 0)
		return 1;
	if ( link_create() != 0)
		return 1;
	if ( !S.daemonize) {
		printf( "%s\n", S.ptsname);
		fflush( stdout);
	}

	if ( S.daemonize && daemon( 0, 0) != 0) {
		kb_log( LOG_ERR, "daemon: %s", strerror( errno));
		link_remove();
		return 1;
	}
	write_pidfile();

	memset( &sa, 0, sizeof( sa));
	sa.sa_handler = on_signal;
	sigemptyset( &sa.sa_mask);
	sigaction( SIGINT, &sa, NULL);
	sigaction( SIGTERM, &sa, NULL);
	sigaction( SIGUSR1, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction( SIGPIPE, &sa, NULL);
	sigaction( SIGHUP, &sa, NULL);

	kb_log( LOG_NOTICE, "kissbufd %s: pty %s%s%s, TNC buffer %u bytes, fill limit %u bytes (%u%%), ACK timeout %llu s", KB_VERSION, S.ptsname, S.link ? " via " : "", S.link ? S.link : "", S.bufsize, S.limit, S.fill_pct, (unsigned long long)( S.timeout_ms / 1000u));

	serial_try_open( mono_ms());

	while ( !g_quit) {
		struct pollfd pfd[2];
		nfds_t nfds = 1;
		uint64_t now, wait = 1000;
		int r, si = -1;

		now = mono_ms();
		if ( S.sfd < 0 && now >= S.reconnect_at)
			serial_try_open( now);
		check_timeouts( now);
		if ( g_stats) {
			g_stats = 0;
			dump_stats();
		}

		if ( S.trk.head != NULL) {
			uint64_t due = S.trk.head->sent_ms + S.timeout_ms;
			uint64_t d = due > now ? due - now : 0;
			if ( d < wait)
				wait = d;
		}
		if ( S.sfd < 0) {
			uint64_t d = S.reconnect_at > now ? S.reconnect_at - now : 0;
			if ( d < wait)
				wait = d;
		}

		pfd[0].fd = S.ptm;
		pfd[0].events = POLLIN | ( S.hout.len > 0 ? POLLOUT : 0);
		pfd[0].revents = 0;
		if ( S.sfd >= 0) {
			si = 1;
			pfd[1].fd = S.sfd;
			pfd[1].events = POLLIN | ( S.sout.len > 0 ? POLLOUT : 0);
			pfd[1].revents = 0;
			nfds = 2;
		}

		r = poll( pfd, nfds, (int)wait);
		if ( r < 0) {
			if ( errno == EINTR)
				continue;
			kb_log( LOG_ERR, "poll: %s", strerror( errno));
			break;
		}
		if ( r == 0)
			continue;

		/* TNC -> us */
		if ( si >= 0 && ( pfd[si].revents & ( POLLIN | POLLHUP | POLLERR))) {
			for ( ;;) {
				ssize_t n = read( S.sfd, rbuf, sizeof( rbuf));
				ssize_t i;

				if ( n > 0) {
					for ( i = 0; i < n && S.sfd >= 0; i++) {
						size_t fl;
						int k = kiss_decode_byte( &S.sdec, rbuf[i], &fl);
						if ( k == KISS_FRAME)
							handle_tnc_frame( S.sdec.buf, fl);
					}
					if ( S.sfd < 0)
						break;
					continue;
				}
				if ( n == 0) {
					serial_lost( "end of file");
					break;
				}
				if ( errno == EINTR)
					continue;
				if ( errno != EAGAIN && errno != EWOULDBLOCK)
					serial_lost( strerror( errno));
				break;
			}
			if ( S.sfd >= 0 && ( pfd[si].revents & ( POLLHUP | POLLERR | POLLNVAL)))
				serial_lost( "hangup");
		}
		if ( si >= 0 && S.sfd >= 0 && ( pfd[si].revents & POLLOUT))
			(void)serial_flush();

		/* host -> us */
		if ( pfd[0].revents & POLLIN) {
			for ( ;;) {
				ssize_t n = read( S.ptm, rbuf, sizeof( rbuf));
				ssize_t i;

				if ( n > 0) {
					for ( i = 0; i < n; i++) {
						size_t fl;
						int k = kiss_decode_byte( &S.hdec, rbuf[i], &fl);
						if ( k == KISS_FRAME)
							handle_host_frame( S.hdec.buf, fl);
						else if ( k == KISS_GIANT) {
							S.st.giants++;
							kb_log( LOG_WARNING, "giant frame from host dropped");
						}
					}
					continue;
				}
				if ( n < 0 && errno == EINTR)
					continue;
				break;
			}
			pump_tx();
		}
		if ( pfd[0].revents & POLLOUT)
			(void)obuf_flush( S.ptm, &S.hout);
		if ( pfd[0].revents & POLLNVAL) {
			kb_log( LOG_ERR, "pty master became invalid");
			break;
		}
	}

	kb_log( LOG_NOTICE, "shutting down");
	dump_stats();
	link_remove();
	if ( S.pidfile != NULL)
		(void)unlink( S.pidfile);
	if ( S.sfd >= 0) {
		/* Let queued bytes drain briefly; frames on the TNC go anyway. */
		(void)tcdrain( S.sfd);
		close( S.sfd);
	}
	tracker_clear( &S.trk);
	txq_clear( &S.q);
	kiss_decoder_free( &S.hdec);
	kiss_decoder_free( &S.sdec);
	free( S.sout.b);
	free( S.hout.b);
	return 0;
}
