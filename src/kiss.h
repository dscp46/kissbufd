/* SPDX-License-Identifier: MIT */
/*
 * kiss.h - KISS framing helpers.
 */
#ifndef KB_KISS_H
#define KB_KISS_H

#include <stddef.h>
#include <stdint.h>

#define KISS_FEND  0xC0u
#define KISS_FESC  0xDBu
#define KISS_TFEND 0xDCu
#define KISS_TFESC 0xDDu

#define KISS_CMD_DATA	 0x00u
#define KISS_CMD_ACKMODE 0x0Cu
#define KISS_CMD_RETURN	 0xFFu /* full byte, no port nibble */

#define KISS_PORT( type)	     ( (uint8_t)( ( type) >> 4))
#define KISS_CMD( type)	     ( (uint8_t)( ( type) & 0x0Fu))
#define KISS_TYPE( port, cmd) ( (uint8_t)( ( ( ( port) & 0x0Fu) << 4) | ( ( cmd) & 0x0Fu)))

/* kiss_decode_byte() results */
#define KISS_NONE  0 /* nothing yet */
#define KISS_FRAME 1 /* complete frame in d->buf[0 .. *flen) */
#define KISS_GIANT 2 /* frame exceeded decoder capacity, dropped */

struct kiss_decoder {
	uint8_t *buf;
	size_t len;
	size_t max;
	int esc;
	int overflow;
	int synced; /* seen at least one FEND */
};

int kiss_decoder_init( struct kiss_decoder *d, size_t max);
void kiss_decoder_free( struct kiss_decoder *d);
void kiss_decoder_reset( struct kiss_decoder *d);
int kiss_decode_byte( struct kiss_decoder *d, uint8_t c, size_t *flen);

/* Size on the wire (FEND + escaped hdr + escaped data + FEND). */
size_t kiss_encoded_size( const uint8_t *hdr, size_t hlen, const uint8_t *data, size_t dlen);
/* Encode into out; returns bytes written, 0 if cap is too small. */
size_t kiss_encode( uint8_t *out, size_t cap, const uint8_t *hdr, size_t hlen, const uint8_t *data, size_t dlen);

/*
 * ACKMODE frame IDs are two bytes on the wire. The protocol does not
 * define an endianness; we treat them as network order (big endian),
 * i.e. the first byte on the wire is the most significant byte. Explicit
 * shifts keep this independent of the host's byte order.
 */
static inline uint16_t net16_get( const uint8_t *p) {
	return (uint16_t)( ( (uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline void net16_put( uint8_t *p, uint16_t v) {
	p[0] = (uint8_t)( v >> 8);
	p[1] = (uint8_t)( v & 0xFFu);
}

#endif /* KB_KISS_H */
