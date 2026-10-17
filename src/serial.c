/* SPDX-License-Identifier: MIT */
/*
 * serial.c - modem serial port configuration.
 */
#include "compat.h"

#include <sys/types.h>
#include <sys/ioctl.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "serial.h"

static const struct {
	long baud;
	speed_t speed;
} bauds[] = {
	{ 1200, B1200 },
	{ 2400, B2400 },
	{ 4800, B4800 },
	{ 9600, B9600 },
	{ 19200, B19200 },
	{ 38400, B38400 },
#ifdef B57600
	{ 57600, B57600 },
#endif
#ifdef B115200
	{ 115200, B115200 },
#endif
#ifdef B230400
	{ 230400, B230400 },
#endif
#ifdef B460800
	{ 460800, B460800 },
#endif
#ifdef B500000
	{ 500000, B500000 },
#endif
#ifdef B921600
	{ 921600, B921600 },
#endif
#ifdef B1000000
	{ 1000000, B1000000 },
#endif
};

static int lookup_baud( long baud, speed_t *sp) {
	size_t i;

	for ( i = 0; i < sizeof( bauds) / sizeof( bauds[0]); i++) {
		if ( bauds[i].baud == baud) {
			*sp = bauds[i].speed;
			return 0;
		}
	}
	return -1;
}

int serial_baud_supported( long baud) {
	speed_t sp;

	return lookup_baud( baud, &sp) == 0;
}

void serial_cfg_defaults( struct serial_cfg *c) {
	memset( c, 0, sizeof( *c));
	c->baud = 57600;
	c->databits = 8;
	c->parity = 'N';
	c->stopbits = 1;
	c->dtr = 1;
}

/* "8N1", "7E1", "8N2" ... */
int serial_parse_mode( const char *s, struct serial_cfg *c) {
	char p;

	if ( s == NULL || strlen( s) != 3)
		return -1;
	if ( s[0] < '5' || s[0] > '8')
		return -1;
	p = (char)toupper( (unsigned char)s[1]);
	if ( p != 'N' && p != 'E' && p != 'O')
		return -1;
	if ( s[2] != '1' && s[2] != '2')
		return -1;
	c->databits = s[0] - '0';
	c->parity = p;
	c->stopbits = s[2] - '0';
	return 0;
}

int serial_open( const struct serial_cfg *c) {
	struct termios tio;
	speed_t sp;
	int fd, saved;

	if ( lookup_baud( c->baud, &sp) != 0) {
		errno = EINVAL;
		return -1;
	}
	fd = open( c->path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if ( fd < 0)
		return -1;
	if ( tcgetattr( fd, &tio) != 0)
		goto fail;

	cfmakeraw( &tio);
	tio.c_iflag &= ~(tcflag_t)( IXON | IXOFF | IXANY | INPCK);
	tio.c_cflag &= ~(tcflag_t)( CSIZE | PARENB | PARODD | CSTOPB);
#ifdef CRTSCTS
	tio.c_cflag &= ~(tcflag_t)CRTSCTS;
#endif
	tio.c_cflag |= CLOCAL | CREAD;

	switch ( c->databits) {
	case 5: tio.c_cflag |= CS5; break;
	case 6: tio.c_cflag |= CS6; break;
	case 7: tio.c_cflag |= CS7; break;
	default: tio.c_cflag |= CS8; break;
	}
	if ( c->parity == 'E' || c->parity == 'O') {
		tio.c_cflag |= PARENB;
		tio.c_iflag |= INPCK;
		if ( c->parity == 'O')
			tio.c_cflag |= PARODD;
	}
	if ( c->stopbits == 2)
		tio.c_cflag |= CSTOPB;

	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;

	if ( cfsetispeed( &tio, sp) != 0 || cfsetospeed( &tio, sp) != 0)
		goto fail;
	if ( tcsetattr( fd, TCSANOW, &tio) != 0)
		goto fail;

#if defined( TIOCMBIS) && defined( TIOCMBIC) && defined( TIOCM_DTR)
	{
		int bits = TIOCM_DTR | TIOCM_RTS;
		/* Best effort: not all devices (e.g. ptys) support it. */
		(void)ioctl( fd, c->dtr ? TIOCMBIS : TIOCMBIC, &bits);
	}
#endif
#ifdef TIOCEXCL
	(void)ioctl( fd, TIOCEXCL);
#endif
	(void)tcflush( fd, TCIOFLUSH);
	return fd;

fail:
	saved = errno;
	close( fd);
	errno = saved;
	return -1;
}
