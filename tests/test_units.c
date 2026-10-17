/* SPDX-License-Identifier: MIT */
/*
 * Unit tests for the KISS codec, tracker and queue.
 */
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "kiss.h"
#include "tracker.h"
#include "txq.h"

static int failures;

#define CHECK( c) \
	do { \
		if ( !( c)) { \
			fprintf( stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
			failures++; \
		} \
	} while ( 0)

static void test_net16( void) {
	uint8_t b[2];

	net16_put( b, 0x1234);
	CHECK( b[0] == 0x12 && b[1] == 0x34);
	CHECK( net16_get( b) == 0x1234);
	b[0] = 0xAB;
	b[1] = 0xCD;
	CHECK( net16_get( b) == 0xABCD);
}

static void test_kiss_roundtrip( void) {
	struct kiss_decoder d;
	uint8_t hdr[3] = { 0x0C, 0xC0, 0xDB };
	uint8_t data[6] = { 1, 0xC0, 2, 0xDB, 0xDC, 0xDD };
	uint8_t enc[64];
	size_t n, i, fl = 0;
	int frames = 0;

	n = kiss_encode( enc, sizeof( enc), hdr, 3, data, 6);
	CHECK( n == kiss_encoded_size( hdr, 3, data, 6));
	CHECK( n == 2 + 3 + 2 + 6 + 2);
	CHECK( kiss_encode( enc, n - 1, hdr, 3, data, 6) == 0);

	CHECK( kiss_decoder_init( &d, 64) == 0);
	/* garbage before sync must be ignored */
	CHECK( kiss_decode_byte( &d, 0x55, &fl) == KISS_NONE);
	for ( i = 0; i < n; i++) {
		if ( kiss_decode_byte( &d, enc[i], &fl) == KISS_FRAME) {
			frames++;
			CHECK( fl == 9);
			CHECK( memcmp( d.buf, hdr, 3) == 0);
			CHECK( memcmp( d.buf + 3, data, 6) == 0);
		}
	}
	CHECK( frames == 1);
	/* back-to-back FENDs produce no empty frames */
	CHECK( kiss_decode_byte( &d, KISS_FEND, &fl) == KISS_NONE);
	kiss_decoder_free( &d);

	/* giant */
	CHECK( kiss_decoder_init( &d, 4) == 0);
	kiss_decode_byte( &d, KISS_FEND, &fl);
	for ( i = 0; i < 10; i++)
		kiss_decode_byte( &d, 0x41, &fl);
	CHECK( kiss_decode_byte( &d, KISS_FEND, &fl) == KISS_GIANT);
	kiss_decoder_free( &d);
}

static void test_tracker( void) {
	static struct tracker t;
	struct tx_rec *r, *r2;
	uint16_t id;
	uint32_t i, n;

	/* IDs needing escapes are skipped */
	CHECK( !tracker_id_wire_safe( 0xC000));
	CHECK( !tracker_id_wire_safe( 0x12DB));
	CHECK( tracker_id_wire_safe( 0x1234));

	tracker_init( &t, 1000, 0xBFFF);
	CHECK( tracker_peek_id( &t, &id) == 0 && id == 0xBFFF);
	r = tracker_commit( &t, id, 300, 0, 0, 0, 0);
	CHECK( r != NULL);
	/* next would be 0xC000..0xC0FF (unsafe) -> 0xC100? no: 0xC100 high
	 * byte 0xC1 ok, low 0x00 ok */
	CHECK( tracker_peek_id( &t, &id) == 0 && id == 0xC100);

	/* accounting */
	CHECK( tracker_fits( &t, 700));
	CHECK( !tracker_fits( &t, 701));
	r2 = tracker_commit( &t, id, 700, 3, 1, 0xBEEF, 5);
	CHECK( r2 != NULL && t.bytes == 1000 && t.count == 2);
	CHECK( !tracker_fits( &t, 1));
	CHECK( tracker_find( &t, 0xC100) == r2);
	CHECK( r2->host_id == 0xBEEF && r2->port == 3 && r2->host_ack == 1);
	CHECK( tracker_find( &t, 0x4242) == NULL);
	/* duplicate commit rejected */
	CHECK( tracker_commit( &t, 0xC100, 1, 0, 0, 0, 0) == NULL);
	tracker_release( &t, r);
	CHECK( t.bytes == 700 && t.head == r2 && t.tail == r2);
	tracker_release( &t, r2);
	CHECK( t.bytes == 0 && t.count == 0 && t.head == NULL);

	/* conflict check after wrap-around */
	tracker_init( &t, 0xFFFFFFFFu, 0xFFFE);
	CHECK( tracker_commit( &t, 0x0001, 5, 0, 0, 0, 0) != NULL);
	t.next_id = 0xFFFE;
	CHECK( tracker_peek_id( &t, &id) == 0 && id == 0xFFFE);
	tracker_commit( &t, id, 5, 0, 0, 0, 0);
	CHECK( tracker_peek_id( &t, &id) == 0 && id == 0xFFFF);
	tracker_commit( &t, id, 5, 0, 0, 0, 0);
	CHECK( tracker_peek_id( &t, &id) == 0 && id == 0x0000);
	tracker_commit( &t, id, 5, 0, 0, 0, 0);
	/* 0x0001 is in flight: must be skipped */
	CHECK( tracker_peek_id( &t, &id) == 0 && id == 0x0002);
	tracker_clear( &t);

	/* exhaustion */
	tracker_init( &t, 0xFFFFFFFFu, 0);
	n = 0;
	for ( i = 0; i < 65536u; i++) {
		if ( tracker_peek_id( &t, &id) != 0)
			break;
		tracker_commit( &t, id, 1, 0, 0, 0, 0);
		n++;
	}
	CHECK( tracker_peek_id( &t, &id) == -1);
	CHECK( n == 254u * 254u);
	tracker_clear( &t);
	CHECK( t.count == 0);
}

static void test_txq( void) {
	struct txq q;
	uint8_t d[100];

	memset( d, 0x11, sizeof( d));
	txq_init( &q, 2 * ( 100 + TXQ_ITEM_OVERHEAD));
	CHECK( txq_push( &q, TXQ_DATA, 0x00, 0, d, 100) == 0);
	CHECK( txq_push( &q, TXQ_RAW, 0x01, 0, d, 1) == 0);
	CHECK( txq_push( &q, TXQ_DATA, 0x00, 0, d, 100) == -1);
	CHECK( txq_peek( &q)->kind == TXQ_DATA);
	txq_pop( &q);
	CHECK( txq_peek( &q)->kind == TXQ_RAW && txq_peek( &q)->type == 0x01);
	CHECK( txq_push( &q, TXQ_ACKMODE, 0x0C, 0x1234, d, 50) == 0);
	txq_pop( &q);
	CHECK( txq_peek( &q)->host_id == 0x1234);
	txq_clear( &q);
	CHECK( q.count == 0 && q.bytes == 0 && txq_peek( &q) == NULL);
}

struct conf_seen {
	int n;
	char k[8][32];
	char v[8][64];
};

static int conf_collect( const char *key, const char *val, const char *where, void *arg) {
	struct conf_seen *c = arg;

	(void)where;
	if ( c->n >= 8)
		return -1;
	snprintf( c->k[c->n], sizeof( c->k[0]), "%s", key);
	snprintf( c->v[c->n], sizeof( c->v[0]), "%s", val);
	c->n++;
	return strcmp( key, "abort") == 0 ? -1 : 0;
}

static int write_file( const char *path, const char *text) {
	FILE *f = fopen( path, "w");

	if ( f == NULL)
		return -1;
	fputs( text, f);
	fclose( f);
	return 0;
}

static void test_config( void) {
	char path[] = "/tmp/kissbufd-test-XXXXXX";
	struct conf_seen c;
	int fd = mkstemp( path);

	CHECK( fd >= 0);
	if ( fd < 0)
		return;
	close( fd);

	CHECK( write_file( path, "# comment\n\n  device = /dev/ttyACM0  \nbuffer_size=32768 # trailing\npty_link = \"/run/a b#c\"  # quoted\npidfile =\n; also a comment\nserial_mode = 8N1") == 0);
	memset( &c, 0, sizeof( c));
	CHECK( conf_parse_file( path, conf_collect, &c) == 0);
	CHECK( c.n == 5);
	CHECK( !strcmp( c.k[0], "device") && !strcmp( c.v[0], "/dev/ttyACM0"));
	CHECK( !strcmp( c.k[1], "buffer_size") && !strcmp( c.v[1], "32768"));
	CHECK( !strcmp( c.k[2], "pty_link") && !strcmp( c.v[2], "/run/a b#c"));
	CHECK( !strcmp( c.k[3], "pidfile") && !strcmp( c.v[3], ""));
	CHECK( !strcmp( c.k[4], "serial_mode") && !strcmp( c.v[4], "8N1"));

	CHECK( write_file( path, "device /dev/ttyACM0\n") == 0);
	memset( &c, 0, sizeof( c));
	fprintf( stderr, "(expected syntax error follows)\n");
	CHECK( conf_parse_file( path, conf_collect, &c) == -1);

	CHECK( write_file( path, "a = 1\nabort = x\nb = 2\n") == 0);
	memset( &c, 0, sizeof( c));
	CHECK( conf_parse_file( path, conf_collect, &c) == -1 && c.n == 2);

	unlink( path);
	fprintf( stderr, "(expected missing-file error follows)\n");
	CHECK( conf_parse_file( path, conf_collect, &c) == -1);
}

int main( void) {
	test_net16();
	test_kiss_roundtrip();
	test_tracker();
	test_txq();
	test_config();
	if ( failures) {
		fprintf( stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf( "unit tests passed\n");
	return 0;
}
