/* SPDX-License-Identifier: MIT */
/*
 * tracker.c - in-flight frame tracker and ACKMODE ID allocator.
 */
#include "compat.h"

#include <stdlib.h>
#include <string.h>

#include "kiss.h"
#include "tracker.h"

#define BIT_TEST( a, i) ( ( a)[( i) >> 3] & (uint8_t)( 1u << ( ( i) & 7u)))
#define BIT_SET( a, i)  ( ( a)[( i) >> 3] |= (uint8_t)( 1u << ( ( i) & 7u)))
#define BIT_CLR( a, i)  ( ( a)[( i) >> 3] &= (uint8_t)~( 1u << ( ( i) & 7u)))

void tracker_init( struct tracker *t, uint32_t limit, uint16_t first_id) {
	memset( t, 0, sizeof( *t));
	t->limit = limit;
	t->next_id = first_id;
}

/*
 * IDs whose bytes would need KISS escaping (0xC0, 0xDB) are never used.
 * This keeps the per-frame cost independent of the ID and sidesteps any
 * firmware that handles escaped IDs in ACKs poorly. ~1.5% of the ID
 * space is skipped.
 */
int tracker_id_wire_safe( uint16_t id) {
	uint8_t b[2];

	net16_put( b, id);
	return b[0] != KISS_FEND && b[0] != KISS_FESC && b[1] != KISS_FEND && b[1] != KISS_FESC;
}

/*
 * Find the next free ID, starting at the counter, skipping IDs still in
 * flight (conflict check) and IDs that would need escaping.
 * Returns 0 and sets *id, or -1 if the ID space is exhausted.
 */
int tracker_peek_id( const struct tracker *t, uint16_t *id) {
	uint32_t n;
	uint16_t c = t->next_id;

	for ( n = 0; n < 65536u; n++, c++) {
		if ( tracker_id_wire_safe( c) && !BIT_TEST( t->inuse, c)) {
			*id = c;
			return 0;
		}
	}
	return -1;
}

int tracker_fits( const struct tracker *t, uint32_t cost) {
	return cost <= t->limit && t->bytes <= t->limit - cost;
}

struct tx_rec *tracker_commit( struct tracker *t, uint16_t wire_id, uint32_t cost, uint8_t port, int host_ack, uint16_t host_id, uint64_t now_ms) {
	struct tx_rec *r;

	if ( BIT_TEST( t->inuse, wire_id))
		return NULL;
	r = calloc( 1, sizeof( *r));
	if ( r == NULL)
		return NULL;
	r->wire_id = wire_id;
	r->host_id = host_id;
	r->host_ack = host_ack ? 1 : 0;
	r->port = port;
	r->cost = cost;
	r->sent_ms = now_ms;

	r->prev = t->tail;
	if ( t->tail != NULL)
		t->tail->next = r;
	else
		t->head = r;
	t->tail = r;

	BIT_SET( t->inuse, wire_id);
	t->count++;
	t->bytes += cost;
	t->next_id = (uint16_t)( wire_id + 1u);
	return r;
}

struct tx_rec *tracker_find( const struct tracker *t, uint16_t wire_id) {
	struct tx_rec *r;

	if ( !BIT_TEST( t->inuse, wire_id))
		return NULL;
	/* ACKs normally come back in order: the head is the usual hit. */
	for ( r = t->head; r != NULL; r = r->next)
		if ( r->wire_id == wire_id)
			return r;
	return NULL;
}

void tracker_release( struct tracker *t, struct tx_rec *r) {
	if ( r->prev != NULL)
		r->prev->next = r->next;
	else
		t->head = r->next;
	if ( r->next != NULL)
		r->next->prev = r->prev;
	else
		t->tail = r->prev;

	BIT_CLR( t->inuse, r->wire_id);
	t->count--;
	t->bytes -= r->cost;
	free( r);
}

void tracker_clear( struct tracker *t) {
	while ( t->head != NULL)
		tracker_release( t, t->head);
}
