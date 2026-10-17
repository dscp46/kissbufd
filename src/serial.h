/* SPDX-License-Identifier: MIT */
/*
 * serial.h - modem serial port configuration.
 */
#ifndef KB_SERIAL_H
#define KB_SERIAL_H

struct serial_cfg {
	const char *path;
	long baud;
	int databits; /* 5..8 */
	char parity;  /* 'N', 'E', 'O' */
	int stopbits; /* 1, 2 */
	int dtr;      /* 1: assert DTR/RTS (default), 0: drop */
};

void serial_cfg_defaults( struct serial_cfg *c);
int serial_parse_mode( const char *s, struct serial_cfg *c);
int serial_baud_supported( long baud);
int serial_open( const struct serial_cfg *c); /* fd, or -1 + errno */

#endif /* KB_SERIAL_H */
