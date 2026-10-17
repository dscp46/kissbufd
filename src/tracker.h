/* SPDX-License-Identifier: MIT */
/*
 * tracker.h - in-flight (on-TNC) frame tracker and ACKMODE ID allocator.
 *
 * Every frame handed to the TNC gets a unique wire ID from an internal
 * counter. Host-originated ACKMODE frames keep their host ID in the record
 * (translation table) so the ACK can be relayed back with the host's ID.
 */
#ifndef KB_TRACKER_H
#define KB_TRACKER_H

#include <stdint.h>

struct tx_rec {
	struct tx_rec *next;
	struct tx_rec *prev;
	uint64_t sent_ms; /* monotonic ms when queued to serial */
	uint32_t cost;	  /* raw KISS bytes on the wire */
	uint16_t wire_id; /* ID used towards the TNC */
	uint16_t host_id; /* host's ID (valid if host_ack) */
	uint8_t port;	  /* KISS port nibble */
	uint8_t host_ack; /* 1: relay ACK to host */
};

struct tracker {
	struct tx_rec *head; /* oldest */
	struct tx_rec *tail; /* newest */
	uint32_t count;
	uint32_t bytes; /* sum of cost of in-flight frames */
	uint32_t limit; /* max bytes allowed in flight */
	uint16_t next_id;
	uint8_t inuse[65536 / 8];
};

void tracker_init( struct tracker *t, uint32_t limit, uint16_t first_id);
int tracker_id_wire_safe( uint16_t id);
int tracker_peek_id( const struct tracker *t, uint16_t *id);
int tracker_fits( const struct tracker *t, uint32_t cost);
struct tx_rec *tracker_commit( struct tracker *t, uint16_t wire_id, uint32_t cost, uint8_t port, int host_ack, uint16_t host_id, uint64_t now_ms);
struct tx_rec *tracker_find( const struct tracker *t, uint16_t wire_id);
void tracker_release( struct tracker *t, struct tx_rec *r);
void tracker_clear( struct tracker *t);

#endif /* KB_TRACKER_H */
