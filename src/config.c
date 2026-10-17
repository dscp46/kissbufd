/* SPDX-License-Identifier: MIT */
/*
 * config.c - "key = value" configuration file reader.
 *
 * Syntax, one setting per line:
 *	# comment
 *	key = value
 *	key = "value with # or spaces"
 * Blank lines and lines starting with '#' or ';' are ignored. An unquoted
 * '#' preceded by whitespace starts a trailing comment.
 */
#include "compat.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

#define LINE_MAX_LEN 1024

static char *trim( char *s) {
	char *e;

	while ( *s != '\0' && isspace( (unsigned char)*s))
		s++;
	e = s + strlen( s);
	while ( e > s && isspace( (unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

static int valid_key( const char *k) {
	if ( *k == '\0')
		return 0;
	for ( ; *k != '\0'; k++)
		if ( !isalnum( (unsigned char)*k) && *k != '_' && *k != '-')
			return 0;
	return 1;
}

int conf_parse_file( const char *path, conf_cb cb, void *arg) {
	char line[LINE_MAX_LEN + 2];
	char where[LINE_MAX_LEN];
	unsigned long lineno = 0;
	FILE *f;
	int rc = 0;

	f = fopen( path, "r");
	if ( f == NULL) {
		fprintf( stderr, "kissbufd: %s: %s\n", path, strerror( errno));
		return -1;
	}
	while ( fgets( line, sizeof( line), f) != NULL) {
		char *p, *eq, *key, *val;
		size_t len = strlen( line);

		lineno++;
		snprintf( where, sizeof( where), "%s:%lu", path, lineno);
		if ( len > 0 && line[len - 1] != '\n' && !feof( f)) {
			fprintf( stderr, "kissbufd: %s: line too long\n", where);
			rc = -1;
			break;
		}
		p = trim( line);
		if ( *p == '\0' || *p == '#' || *p == ';')
			continue;
		eq = strchr( p, '=');
		if ( eq == NULL) {
			fprintf( stderr, "kissbufd: %s: expected key = value\n", where);
			rc = -1;
			break;
		}
		*eq = '\0';
		key = trim( p);
		val = trim( eq + 1);
		if ( !valid_key( key)) {
			fprintf( stderr, "kissbufd: %s: bad key\n", where);
			rc = -1;
			break;
		}
		if ( *val == '"' || *val == '\'') {
			char q = *val;
			char *end = strchr( val + 1, q);

			if ( end == NULL) {
				fprintf( stderr, "kissbufd: %s: unterminated quote\n", where);
				rc = -1;
				break;
			}
			*end = '\0';
			p = trim( end + 1);
			if ( *p != '\0' && *p != '#') {
				fprintf( stderr, "kissbufd: %s: garbage after quoted value\n", where);
				rc = -1;
				break;
			}
			val = val + 1;
		} else {
			/* strip " # trailing comment" */
			for ( p = val; *p != '\0'; p++) {
				if ( *p == '#' && ( p == val || isspace( (unsigned char)p[-1]))) {
					*p = '\0';
					break;
				}
			}
			val = trim( val);
		}
		if ( cb( key, val, where, arg) != 0) {
			rc = -1;
			break;
		}
	}
	if ( rc == 0 && ferror( f)) {
		fprintf( stderr, "kissbufd: %s: read error\n", path);
		rc = -1;
	}
	fclose( f);
	return rc;
}
