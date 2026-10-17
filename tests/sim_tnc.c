/* SPDX-License-Identifier: MIT */
/*
 * End-to-end test for kissbufd against a simulated ACKMODE KISS TNC.
 *
 * The simulated TNC sits on a pty pair, has a fixed transmit buffer counted
 * in raw KISS bytes, "transmits" frames at a configurable air rate and
 * returns an ACKMODE ACK when each frame has left. The test fails if its
 * buffer ever overflows. The test also plays the host program on the pty
 * kissbufd exposes.
 *
 * usage: sim_tnc path/to/kissbufd   (KB_TEST_BUF sets the TNC buffer size)
 */
#include "compat.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define FEND  0xC0
#define FESC  0xDB
#define TFEND 0xDC
#define TFESC 0xDD

#define MAXF 70000
#define MAXQ 4096

/* ------------------------------------------------------------------ */
/* helpers                                                            */

static int failures;

static void check( int cond, const char *msg) {
	printf( "  %s %s\n", cond ? "ok  " : "FAIL", msg);
	if ( !cond)
		failures++;
}

static double now_s( void) {
	struct timespec ts;

	clock_gettime( CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *xmalloc( size_t n) {
	void *p = malloc( n > 0 ? n : 1);

	if ( p == NULL) {
		perror( "malloc");
		exit( 2);
	}
	return p;
}

static uint32_t rnd_state = 42;

static uint32_t rnd( void) {
	rnd_state = rnd_state * 1103515245u + 12345u;
	return rnd_state >> 8;
}

static uint32_t rnd_range( uint32_t lo, uint32_t hi) {
	return lo + rnd() % ( hi - lo + 1);
}

/* growable list of byte strings */
struct blob {
	uint8_t *b;
	size_t n;
};

struct list {
	struct blob *v;
	size_t n, cap;
};

static void list_add( struct list *l, const uint8_t *b, size_t n) {
	if ( l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 64;
		l->v = realloc( l->v, l->cap * sizeof( *l->v));
		if ( l->v == NULL)
			exit( 2);
	}
	l->v[l->n].b = xmalloc( n);
	memcpy( l->v[l->n].b, b, n);
	l->v[l->n].n = n;
	l->n++;
}

static void list_add2( struct list *l, uint8_t tag, const uint8_t *b, size_t n) {
	uint8_t *t = xmalloc( n + 1);

	t[0] = tag;
	memcpy( t + 1, b, n);
	list_add( l, t, n + 1);
	free( t);
}

static void list_clear( struct list *l) {
	size_t i;

	for ( i = 0; i < l->n; i++)
		free( l->v[i].b);
	l->n = 0;
}

static void list_free( struct list *l) {
	list_clear( l);
	free( l->v);
	l->v = NULL;
	l->cap = 0;
}

/* Compare l[from..] with r[0..]. */
static int list_eq( const struct list *l, size_t from, const struct list *r) {
	size_t i;

	if ( l->n < from || l->n - from != r->n)
		return 0;
	for ( i = 0; i < r->n; i++)
		if ( l->v[from + i].n != r->v[i].n || memcmp( l->v[from + i].b, r->v[i].b, r->v[i].n) != 0)
			return 0;
	return 1;
}

/* ------------------------------------------------------------------ */
/* KISS                                                               */

static size_t kiss_enc( uint8_t *out, const uint8_t *f, size_t n) {
	size_t i, o = 0;

	out[o++] = FEND;
	for ( i = 0; i < n; i++) {
		if ( f[i] == FEND) {
			out[o++] = FESC;
			out[o++] = TFEND;
		} else if ( f[i] == FESC) {
			out[o++] = FESC;
			out[o++] = TFESC;
		} else
			out[o++] = f[i];
	}
	out[o++] = FEND;
	return o;
}

static size_t kiss_size( const uint8_t *f, size_t n) {
	size_t i, s = 2;

	for ( i = 0; i < n; i++)
		s += ( f[i] == FEND || f[i] == FESC) ? 2 : 1;
	return s;
}

struct dec {
	uint8_t buf[MAXF];
	size_t len;
	int esc, synced;
};

/* Returns 1 when a frame is complete in d->buf[0..*len). */
static int dec_byte( struct dec *d, uint8_t c, size_t *len) {
	if ( c == FEND) {
		int r = d->synced && d->len > 0;

		*len = d->len;
		d->synced = 1;
		d->len = 0;
		d->esc = 0;
		return r;
	}
	if ( !d->synced)
		return 0;
	if ( d->esc) {
		d->esc = 0;
		if ( c == TFEND)
			c = FEND;
		else if ( c == TFESC)
			c = FESC;
	} else if ( c == FESC) {
		d->esc = 1;
		return 0;
	}
	if ( d->len < MAXF)
		d->buf[d->len++] = c;
	return 0;
}

static int open_pty( int *m, int *s) {
	struct termios tio;

	*m = posix_openpt( O_RDWR | O_NOCTTY);
	if ( *m < 0 || grantpt( *m) != 0 || unlockpt( *m) != 0)
		return -1;
	*s = open( ptsname( *m), O_RDWR | O_NOCTTY);
	if ( *s < 0)
		return -1;
	if ( tcgetattr( *s, &tio) == 0) {
		cfmakeraw( &tio);
		tcsetattr( *s, TCSANOW, &tio);
	}
	/* kissbufd must not inherit the TNC side, or closing it would not hang up the line */
	fcntl( *m, F_SETFD, FD_CLOEXEC);
	fcntl( *s, F_SETFD, FD_CLOEXEC);
	return fcntl( *m, F_SETFL, fcntl( *m, F_GETFL) | O_NONBLOCK);
}

/* ------------------------------------------------------------------ */
/* simulated TNC                                                      */

struct air {
	uint16_t wid;
	uint8_t port;
	uint8_t *p;
	size_t n;
	size_t cost;
};

static struct sim {
	int m, s;
	char link[PATH_MAX];
	struct dec dec;
	size_t capacity, used, peak;
	double bps;
	int overflows, dup_ids, plain_seen;
	struct air q[MAXQ];
	size_t qn;
	int txing;
	double tx_end;
	uint8_t inflight[65536];
	struct list received; /* 'D' + payload, or 'C' + type + params */
	struct list sent_air; /* payloads, in transmission order */
	int drop_acks;
	int hold;
} sim;

static void sim_write( const uint8_t *f, size_t n) {
	uint8_t buf[MAXF * 2 + 2];
	size_t len = kiss_enc( buf, f, n);

	if ( sim.m >= 0 && write( sim.m, buf, len) != (ssize_t)len)
		fprintf( stderr, "sim: short write\n");
}

static void sim_plug( void) {
	char tmp[PATH_MAX + 8];

	if ( open_pty( &sim.m, &sim.s) != 0) {
		perror( "sim pty");
		exit( 2);
	}
	snprintf( tmp, sizeof( tmp), "%s.new", sim.link);
	unlink( tmp);
	if ( symlink( ptsname( sim.m), tmp) != 0 || rename( tmp, sim.link) != 0) {
		perror( "sim link");
		exit( 2);
	}
	memset( &sim.dec, 0, sizeof( sim.dec));
}

/* USB unplug: the TNC and its buffer vanish. */
static void sim_unplug( void) {
	size_t i;

	close( sim.s);
	close( sim.m);
	sim.m = sim.s = -1;
	unlink( sim.link);
	for ( i = 0; i < sim.qn; i++)
		free( sim.q[i].p);
	sim.qn = 0;
	sim.used = 0;
	sim.txing = 0;
	memset( sim.inflight, 0, sizeof( sim.inflight));
}

static void sim_frame( const uint8_t *f, size_t n) {
	uint8_t t = f[0];

	if ( t != 0xFF && ( t & 0x0F) == 0x0C && n >= 3) {
		struct air *a;
		uint16_t wid = (uint16_t)( ( f[1] << 8) | f[2]);

		list_add2( &sim.received, 'D', f + 3, n - 3);
		if ( sim.inflight[wid])
			sim.dup_ids++;
		sim.inflight[wid] = 1;
		if ( sim.qn >= MAXQ) {
			fprintf( stderr, "sim: queue overflow\n");
			exit( 2);
		}
		a = &sim.q[sim.qn++];
		a->wid = wid;
		a->port = t >> 4;
		a->n = n - 3;
		a->p = xmalloc( a->n);
		memcpy( a->p, f + 3, a->n);
		a->cost = kiss_size( f, n);
		sim.used += a->cost;
		if ( sim.used > sim.peak)
			sim.peak = sim.used;
		if ( sim.used > sim.capacity)
			sim.overflows++;
		return;
	}
	if ( t != 0xFF && ( t & 0x0F) == 0x00)
		sim.plain_seen++;
	list_add2( &sim.received, 'C', f, n);
}

static void sim_step( double now) {
	uint8_t buf[8192];
	ssize_t r, i;
	size_t len;

	if ( sim.m < 0)
		return;
	while ( ( r = read( sim.m, buf, sizeof( buf))) > 0)
		for ( i = 0; i < r; i++)
			if ( dec_byte( &sim.dec, buf[i], &len))
				sim_frame( sim.dec.buf, len);
	if ( sim.hold)
		return;
	if ( !sim.txing && sim.qn > 0) {
		sim.tx_end = now + (double)( sim.q[0].n + 20) * 8.0 / sim.bps;
		sim.txing = 1;
	}
	if ( sim.txing && now >= sim.tx_end) {
		struct air a = sim.q[0];

		memmove( sim.q, sim.q + 1, ( sim.qn - 1) * sizeof( sim.q[0]));
		sim.qn--;
		sim.txing = 0;
		sim.used -= a.cost;
		sim.inflight[a.wid] = 0;
		list_add( &sim.sent_air, a.p, a.n);
		free( a.p);
		if ( sim.drop_acks > 0)
			sim.drop_acks--;
		else {
			uint8_t ack[3];

			ack[0] = (uint8_t)( ( a.port << 4) | 0x0C);
			ack[1] = (uint8_t)( a.wid >> 8);
			ack[2] = (uint8_t)( a.wid & 0xFF);
			sim_write( ack, 3);
		}
	}
}

/* ------------------------------------------------------------------ */
/* host side                                                          */

static struct host {
	int fd;
	struct dec dec;
	struct list rx;
	uint8_t *out;
	size_t outn, outcap;
} host;

static void host_open( const char *path) {
	struct termios tio;

	host.fd = open( path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if ( host.fd < 0) {
		perror( path);
		exit( 2);
	}
	if ( tcgetattr( host.fd, &tio) == 0) {
		cfmakeraw( &tio);
		tcsetattr( host.fd, TCSANOW, &tio);
	}
}

static void host_send( const uint8_t *f, size_t n) {
	size_t need = 2 * n + 2;

	if ( host.outn + need > host.outcap) {
		host.outcap = ( host.outn + need) * 2;
		host.out = realloc( host.out, host.outcap);
		if ( host.out == NULL)
			exit( 2);
	}
	host.outn += kiss_enc( host.out + host.outn, f, n);
}

static void host_step( void) {
	uint8_t buf[8192];
	ssize_t r, i;
	size_t len;

	if ( host.outn > 0) {
		r = write( host.fd, host.out, host.outn > 4096 ? 4096 : host.outn);
		if ( r > 0) {
			memmove( host.out, host.out + r, host.outn - (size_t)r);
			host.outn -= (size_t)r;
		}
	}
	while ( ( r = read( host.fd, buf, sizeof( buf))) > 0)
		for ( i = 0; i < r; i++)
			if ( dec_byte( &host.dec, buf[i], &len))
				list_add( &host.rx, host.dec.buf, len);
}

/* ------------------------------------------------------------------ */
/* event loop                                                         */

typedef int ( *cond_fn)( void);

static size_t want_air, want_rx, air_base;

static int cond_air_rx( void) {
	return sim.sent_air.n - air_base >= want_air && host.rx.n >= want_rx;
}

static int cond_rx( void) {
	return host.rx.n >= want_rx;
}

static int run( double secs, cond_fn until) {
	double end = now_s() + secs;

	while ( now_s() < end) {
		struct pollfd p[2];
		nfds_t n = 1;

		sim_step( now_s());
		host_step();
		if ( until != NULL && until())
			return 1;
		p[0].fd = host.fd;
		p[0].events = POLLIN;
		if ( sim.m >= 0) {
			p[1].fd = sim.m;
			p[1].events = POLLIN;
			n = 2;
		}
		poll( p, n, 2);
	}
	return until != NULL ? until() : 1;
}

/* ------------------------------------------------------------------ */
/* running kissbufd                                                   */

/* Run prog with args, capturing stdout/stderr into files; returns status. */
static int run_capture( char *const argv[], const char *out, const char *err) {
	pid_t pid = fork();
	int st;

	if ( pid == 0) {
		int o = open( out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		int e = open( err, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		dup2( o, 1);
		dup2( e, 2);
		execv( argv[0], argv);
		_exit( 127);
	}
	if ( pid < 0 || waitpid( pid, &st, 0) < 0)
		return -1;
	return WIFEXITED( st) ? WEXITSTATUS( st) : -1;
}

static char *read_file( const char *path) {
	FILE *f = fopen( path, "r");
	size_t n = 0, cap = 4096, r;
	char *b = xmalloc( cap + 1);

	if ( f == NULL) {
		b[0] = '\0';
		return b;
	}
	while ( ( r = fread( b + n, 1, cap - n, f)) > 0) {
		n += r;
		if ( n == cap) {
			cap *= 2;
			b = realloc( b, cap + 1);
			if ( b == NULL)
				exit( 2);
		}
	}
	fclose( f);
	b[n] = '\0';
	return b;
}

static size_t count_str( const char *hay, const char *needle) {
	size_t n = 0;

	while ( ( hay = strstr( hay, needle)) != NULL) {
		n++;
		hay++;
	}
	return n;
}

/* ------------------------------------------------------------------ */

int main( int argc, char **argv) {
	char dir[] = "/tmp/kissbufd-test-XXXXXX";
	char conf[PATH_MAX], bad[PATH_MAX], hostlink[PATH_MAX], logpath[PATH_MAX], outp[PATH_MAX], errp[PATH_MAX];
	const char *prog = argc > 1 ? argv[1] : "./kissbufd";
	const char *env = getenv( "KB_TEST_BUF");
	size_t buf = env != NULL ? (size_t)strtoul( env, NULL, 10) : 8192;
	size_t limit = buf * 95 / 100;
	struct list expected_air = { 0 }, expected_order = { 0 }, expected_acks = { 0 }, frames = { 0 };
	uint8_t *f = xmalloc( MAXF);
	char msg[256], *text, *cerr, *cout;
	struct stat sb;
	size_t i, on_tnc;
	pid_t pid;
	double t0;
	FILE *cf;
	int st, logfd, ok;

	if ( mkdtemp( dir) == NULL) {
		perror( "mkdtemp");
		return 2;
	}
	snprintf( sim.link, sizeof( sim.link), "%s/tnc", dir);
	snprintf( hostlink, sizeof( hostlink), "%s/kiss", dir);
	snprintf( logpath, sizeof( logpath), "%s/daemon.log", dir);
	snprintf( conf, sizeof( conf), "%s/test.conf", dir);
	snprintf( bad, sizeof( bad), "%s/bad.conf", dir);
	snprintf( outp, sizeof( outp), "%s/out", dir);
	snprintf( errp, sizeof( errp), "%s/err", dir);
	signal( SIGPIPE, SIG_IGN);

	sim.capacity = buf;
	sim.bps = 200000;
	sim_plug();

	/* Settings come from a config file; fill_percent is deliberately wrong there and overridden on the command line, to test precedence. */
	cf = fopen( conf, "w");
	fprintf( cf, "# test configuration\ndevice = %s\nbuffer_size = %zu\nfill_percent = 50\npty_link = \"%s\"   # host side\nack_timeout = 3\nreopen_interval = 1\nverbose = 2\n", sim.link, buf, hostlink);
	fclose( cf);
	cf = fopen( bad, "w");
	fprintf( cf, "device = /dev/null\nbogus_key = 1\n");
	fclose( cf);

	printf( "0. configuration\n");
	{
		char *a1[] = { (char *)prog, "-n", "-c", conf, NULL };
		char *a2[] = { (char *)prog, "-n", "-c", bad, NULL };

		st = run_capture( a1, outp, errp);
		cout = read_file( outp);
		check( st == 0 && strstr( cout, "configuration OK") != NULL, "config check (-n) accepts the test config");
		free( cout);
		st = run_capture( a2, outp, errp);
		cerr = read_file( errp);
		check( st == 2 && strstr( cerr, "bad.conf:2: bogus_key") != NULL, "unknown key rejected with file:line");
		free( cerr);
	}

	logfd = open( logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	pid = fork();
	if ( pid == 0) {
		int null = open( "/dev/null", O_WRONLY);

		dup2( null, 1);
		dup2( logfd, 2);
		execl( prog, prog, "-c", conf, "-F", "95", (char *)NULL);
		_exit( 127);
	}
	for ( i = 0; i < 200 && lstat( hostlink, &sb) != 0; i++)
		usleep( 10000);
	host_open( hostlink);
	run( 0.3, NULL);

	/* ------------------------------------------------------------ */
	printf( "1. burst with mixed plain/ACKMODE frames, commands, a giant\n");
	f[0] = 0x01;
	f[1] = 30; /* TXDELAY */
	host_send( f, 2);
	list_add2( &expected_order, 'C', f, 2);
	for ( i = 0; i < 160; i++) {
		size_t size = rnd_range( 16, 900), k;
		uint8_t *p = f + 3;

		for ( k = 0; k < size; k++)
			p[k] = (uint8_t)rnd();
		if ( i % 25 == 3) {
			memmove( p + 50, p + 10, size - 10);
			for ( k = 0; k < 40; k++)
				p[10 + k] = ( k & 1) ? FESC : FEND;
			size += 40;
		}
		if ( i == 50) {
			uint8_t *g = calloc( 1, limit + 1); /* giant: must be dropped */

			host_send( g, limit + 1);
			free( g);
		}
		if ( i == 75) {
			uint8_t c[2] = { 0x02, 63 }; /* persistence */

			host_send( c, 2);
			list_add2( &expected_order, 'C', c, 2);
		}
		if ( i % 2 == 0) {
			f[2] = 0x00;
			host_send( f + 2, size + 1);
		} else {
			static const uint16_t ids[] = { 0x0007, 0xC0C0, 0xDBDB, 0x0000 };
			uint32_t pick = rnd_range( 0, 4);
			uint16_t hid = pick < 4 ? ids[pick] : (uint16_t)rnd();

			f[0] = 0x0C;
			f[1] = (uint8_t)( hid >> 8);
			f[2] = (uint8_t)( hid & 0xFF);
			host_send( f, size + 3);
			list_add( &expected_acks, f, 3);
		}
		list_add( &expected_air, p, size);
		list_add2( &expected_order, 'D', p, size);
	}
	air_base = 0;
	want_air = expected_air.n;
	want_rx = expected_acks.n;
	ok = run( 30, cond_air_rx);
	run( 0.3, NULL);
	check( ok, "all frames transmitted and acked within time");
	snprintf( msg, sizeof( msg), "TNC buffer never overflowed (peak %zu / capacity %zu, limit %zu)", sim.peak, buf, limit);
	check( sim.overflows == 0, msg);
	check( sim.peak <= limit, "peak never exceeded 95% fill limit");
	snprintf( msg, sizeof( msg), "buffer was actually kept full (peak %zu)", sim.peak);
	check( sim.peak + 1000 >= limit, msg);
	check( list_eq( &sim.sent_air, 0, &expected_air), "payloads arrived intact and in order (giant dropped)");
	check( list_eq( &sim.received, 0, &expected_order), "commands passed through in arrival order");
	check( sim.plain_seen == 0, "plain frames converted to ACKMODE");
	check( sim.dup_ids == 0, "no wire ID reused while in flight");
	snprintf( msg, sizeof( msg), "host got exactly its own ACKs, with its own IDs, in order (%zu)", expected_acks.n);
	check( list_eq( &host.rx, 0, &expected_acks), msg);
	list_clear( &host.rx);

	/* ------------------------------------------------------------ */
	printf( "2. unknown ACK and received packet relay\n");
	{
		static const uint8_t unk[] = { 0x0C, 0x77, 0x77 };
		static const uint8_t pkt[] = { 0x00, 0x96, 0x70, 0x9a, 0x9a, 0x9e, 0x40, 0xe0, 'h', 'e', 'l', 'l', 'o' };
		struct list want = { 0 };

		sim_write( unk, sizeof( unk));
		sim_write( pkt, sizeof( pkt));
		want_rx = 1;
		run( 0.5, cond_rx);
		run( 0.2, NULL);
		list_add( &want, pkt, sizeof( pkt));
		check( list_eq( &host.rx, 0, &want), "unknown ACK discarded, received frame relayed");
		list_free( &want);
		list_clear( &host.rx);
	}

	/* ------------------------------------------------------------ */
	printf( "3. lost ACK -> timeout frees buffer, host not notified\n");
	{
		size_t big = limit * 3 / 4;
		static const uint8_t ack2[] = { 0x0C, 0x43, 0x43 };
		struct list want = { 0 };

		sim.drop_acks = 1;
		f[0] = 0x0C;
		f[1] = 0x42;
		f[2] = 0x42;
		memset( f + 3, 0x11, big);
		host_send( f, big + 3);
		f[1] = 0x43;
		f[2] = 0x43;
		memset( f + 3, 0x22, big);
		host_send( f, big + 3);
		t0 = now_s();
		want_rx = 1;
		ok = run( 8, cond_rx);
		list_add( &want, ack2, sizeof( ack2));
		check( ok && list_eq( &host.rx, 0, &want), "only the second frame's ACK reached the host");
		snprintf( msg, sizeof( msg), "second frame held until timeout (%.1fs)", now_s() - t0);
		check( now_s() - t0 >= 2.8, msg);
		list_free( &want);
		list_clear( &host.rx);
	}

	/* ------------------------------------------------------------ */
	printf( "4. TNC unplug: on-TNC frames lost, host queue held and resent\n");
	{
		size_t n_before = sim.sent_air.n, fl = limit / 16;
		struct list want_acks = { 0 }, want_air2 = { 0 };

		sim.hold = 1;
		for ( i = 0; i < 30; i++) {
			f[0] = 0x0C;
			f[1] = 0x50;
			f[2] = (uint8_t)i;
			memset( f + 3, 0x30 + (int)( i % 10), fl);
			list_add( &frames, f + 3, fl);
			host_send( f, fl + 3);
		}
		run( 0.5, NULL);
		on_tnc = sim.qn;
		snprintf( msg, sizeof( msg), "%zu frames on TNC, rest held by kissbufd", on_tnc);
		check( on_tnc > 0 && on_tnc < 30, msg);
		sim_unplug();
		run( 0.5, NULL);
		sim.hold = 0;
		sim_plug();
		air_base = n_before;
		want_air = 30 - on_tnc;
		want_rx = 30 - on_tnc;
		ok = run( 10, cond_air_rx);
		run( 0.3, NULL);
		for ( i = on_tnc; i < 30; i++) {
			uint8_t a[3] = { 0x0C, 0x50, (uint8_t)i };

			list_add( &want_air2, frames.v[i].b, frames.v[i].n);
			list_add( &want_acks, a, 3);
		}
		check( ok && list_eq( &sim.sent_air, n_before, &want_air2), "held frames sent after reconnect, lost ones not resent");
		check( list_eq( &host.rx, 0, &want_acks), "ACKs only for frames that really went out");
		check( sim.overflows == 0, "no overflow after reconnect");
		list_free( &want_acks);
		list_free( &want_air2);
	}

	/* ------------------------------------------------------------ */
	kill( pid, SIGTERM);
	waitpid( pid, &st, 0);
	close( logfd);
	text = read_file( logpath);
	check( strstr( text, "MINOR ALARM") != NULL, "timeout logged as minor alarm");
	check( strstr( text, "giant frame") != NULL, "giant drop logged");
	check( strstr( text, "TNC link lost") != NULL && count_str( text, "TNC link up") >= 2, "link loss and recovery logged");
	check( lstat( hostlink, &sb) != 0, "pty symlink removed on exit");
	free( text);
	list_free( &expected_air);
	list_free( &expected_order);
	list_free( &expected_acks);
	list_free( &frames);
	list_free( &sim.received);
	list_free( &sim.sent_air);
	list_free( &host.rx);
	for ( i = 0; i < sim.qn; i++)
		free( sim.q[i].p);
	free( host.out);
	free( f);

	if ( failures) {
		printf( "\n%d FAILURE(S); daemon log: %s\n", failures, logpath);
		return 1;
	}
	printf( "\nintegration tests passed\n");
	return 0;
}
