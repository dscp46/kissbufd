/* SPDX-License-Identifier: MIT */
/*
 * compat.h - feature test macros. Must be the first include of every .c file.
 *
 * glibc/musl hide posix_openpt(), cfmakeraw(), daemon() unless asked.
 * The BSDs and macOS expose everything by default, and defining
 * _XOPEN_SOURCE there would *hide* BSD extensions, so we leave them alone.
 */
#ifndef KB_COMPAT_H
#define KB_COMPAT_H

#if defined( __linux__) || defined( __CYGWIN__) || defined( __GNU__)
# ifndef _GNU_SOURCE
#  define _GNU_SOURCE 1
# endif
#endif

#endif /* KB_COMPAT_H */
