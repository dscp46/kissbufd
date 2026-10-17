/* SPDX-License-Identifier: MIT */
/*
 * config.h - "key = value" configuration file reader.
 */
#ifndef KB_CONFIG_H
#define KB_CONFIG_H

/*
 * Called for each setting. `where` is "file:line" for diagnostics.
 * Return 0 to continue, -1 to abort parsing.
 */
typedef int ( *conf_cb)( const char *key, const char *val, const char *where, void *arg);

/* Returns 0 on success, -1 on I/O or syntax error (reported on stderr). */
int conf_parse_file( const char *path, conf_cb cb, void *arg);

#endif /* KB_CONFIG_H */
