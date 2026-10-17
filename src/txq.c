/* SPDX-License-Identifier: MIT */
/*
 * txq.c - FIFO of frames received from the host.
 */
#include "compat.h"

#include <stdlib.h>
#include <string.h>

#include "txq.h"

void txq_init( struct txq *q, size_t limit) {
	memset( q, 0, sizeof( *q));
	q->limit = limit;
}

/* Returns 0 on success, -1 if the queue is full, -2 on allocation failure. */
int txq_push( struct txq *q, enum txq_kind kind, uint8_t type, uint16_t host_id, const uint8_t *data, size_t len) {
	struct txq_item *it;
	size_t cost = len + TXQ_ITEM_OVERHEAD;

	if ( cost > q->limit || q->bytes > q->limit - cost)
		return -1;
	it = malloc( sizeof( *it) + len);
	if ( it == NULL)
		return -2;
	it->next = NULL;
	it->len = len;
	it->host_id = host_id;
	it->type = type;
	it->kind = (uint8_t)kind;
	if ( len > 0)
		memcpy( it->data, data, len);

	if ( q->tail != NULL)
		q->tail->next = it;
	else
		q->head = it;
	q->tail = it;
	q->bytes += cost;
	q->count++;
	return 0;
}

struct txq_item *txq_peek( const struct txq *q) {
	return q->head;
}

void txq_pop( struct txq *q) {
	struct txq_item *it = q->head;

	if ( it == NULL)
		return;
	q->head = it->next;
	if ( q->head == NULL)
		q->tail = NULL;
	q->bytes -= it->len + TXQ_ITEM_OVERHEAD;
	q->count--;
	free( it);
}

void txq_clear( struct txq *q) {
	while ( q->head != NULL)
		txq_pop( q);
}
