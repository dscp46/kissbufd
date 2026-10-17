/* SPDX-License-Identifier: MIT */
/*
 * txq.h - FIFO of frames received from the host, waiting for TNC room.
 */
#ifndef KB_TXQ_H
#define KB_TXQ_H

#include <stddef.h>
#include <stdint.h>

enum txq_kind {
	TXQ_DATA = 0, /* plain KISS data: gets an internal ACKMODE id */
	TXQ_ACKMODE,  /* host ACKMODE frame: host_id is translated */
	TXQ_RAW	      /* other command: passed through as-is */
};

struct txq_item {
	struct txq_item *next;
	size_t len; /* bytes in data[] */
	uint16_t host_id;
	uint8_t type; /* original KISS type byte */
	uint8_t kind;
	uint8_t data[]; /* payload after type (and host id) */
};

struct txq {
	struct txq_item *head;
	struct txq_item *tail;
	size_t bytes;
	size_t limit;
	uint32_t count;
};

#define TXQ_ITEM_OVERHEAD 8u

void txq_init( struct txq *q, size_t limit);
int txq_push( struct txq *q, enum txq_kind kind, uint8_t type, uint16_t host_id, const uint8_t *data, size_t len);
struct txq_item *txq_peek( const struct txq *q);
void txq_pop( struct txq *q);
void txq_clear( struct txq *q);

#endif /* KB_TXQ_H */
