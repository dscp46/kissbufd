/* SPDX-License-Identifier: MIT */
/*
 * kiss.c - KISS framing helpers.
 */
#include "compat.h"

#include <stdlib.h>
#include <string.h>

#include "kiss.h"

int kiss_decoder_init( struct kiss_decoder *d, size_t max) {
	memset( d, 0, sizeof( *d));
	d->buf = malloc( max > 0 ? max : 1);
	if ( d->buf == NULL)
		return -1;
	d->max = max;
	return 0;
}

void kiss_decoder_free( struct kiss_decoder *d) {
	free( d->buf);
	d->buf = NULL;
}

void kiss_decoder_reset( struct kiss_decoder *d) {
	d->len = 0;
	d->esc = 0;
	d->overflow = 0;
	d->synced = 0;
}

/*
 * Feed one byte. On KISS_FRAME the frame is in d->buf[0..*flen) and stays
 * valid until the next call.
 */
int kiss_decode_byte( struct kiss_decoder *d, uint8_t c, size_t *flen) {
	int r = KISS_NONE;

	if ( c == KISS_FEND) {
		if ( d->synced) {
			if ( d->overflow)
				r = KISS_GIANT;
			else if ( d->len > 0) {
				*flen = d->len;
				r = KISS_FRAME;
			}
		}
		d->synced = 1;
		d->len = 0;
		d->esc = 0;
		d->overflow = 0;
		return r;
	}
	if ( !d->synced)
		return KISS_NONE; /* line noise before first FEND */

	if ( d->esc) {
		d->esc = 0;
		if ( c == KISS_TFEND)
			c = KISS_FEND;
		else if ( c == KISS_TFESC)
			c = KISS_FESC;
		/* else: protocol error, keep the byte as-is */
	} else if ( c == KISS_FESC) {
		d->esc = 1;
		return KISS_NONE;
	}

	if ( d->overflow)
		return KISS_NONE;
	if ( d->len >= d->max) {
		d->overflow = 1;
		return KISS_NONE;
	}
	d->buf[d->len++] = c;
	return KISS_NONE;
}

static size_t esc_len( const uint8_t *p, size_t n) {
	size_t i, r = 0;

	for ( i = 0; i < n; i++)
		r += ( p[i] == KISS_FEND || p[i] == KISS_FESC) ? 2 : 1;
	return r;
}

size_t kiss_encoded_size( const uint8_t *hdr, size_t hlen, const uint8_t *data, size_t dlen) {
	return 2 + esc_len( hdr, hlen) + esc_len( data, dlen);
}

static size_t esc_copy( uint8_t *out, const uint8_t *p, size_t n) {
	size_t i, o = 0;

	for ( i = 0; i < n; i++) {
		if ( p[i] == KISS_FEND) {
			out[o++] = KISS_FESC;
			out[o++] = KISS_TFEND;
		} else if ( p[i] == KISS_FESC) {
			out[o++] = KISS_FESC;
			out[o++] = KISS_TFESC;
		} else
			out[o++] = p[i];
	}
	return o;
}

size_t kiss_encode( uint8_t *out, size_t cap, const uint8_t *hdr, size_t hlen, const uint8_t *data, size_t dlen) {
	size_t o, need = kiss_encoded_size( hdr, hlen, data, dlen);

	if ( need > cap)
		return 0;
	o = 0;
	out[o++] = KISS_FEND;
	o += esc_copy( out + o, hdr, hlen);
	o += esc_copy( out + o, data, dlen);
	out[o++] = KISS_FEND;
	return o;
}
