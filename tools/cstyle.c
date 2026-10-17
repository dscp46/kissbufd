/* SPDX-License-Identifier: MIT */
/*
 * cstyle - the parts of the kissbufd C style clang-format cannot express.
 *
 * Used as two filters around clang-format (see "make style"):
 *
 *   cstyle -1 < in.c | clang-format ... | cstyle -2 > out.c
 *
 * -1  joins adjacent string literals ("a" "b" and "a"<newline>"b") into one
 *     literal, so clang-format has nothing to split. Regions between
 *     "clang-format off" and "clang-format on" comments are left alone.
 * -2  puts a space after every opening parenthesis and none before the
 *     closing one, except for casts and empty parentheses:
 *         int f( void) {   if ( x == 1) {   foo( a, b);   y = (uint8_t)x;
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OFF_MARK "/* clang-format off */"
#define ON_MARK	 "/* clang-format on */"

enum tail {
	TAIL_NONE,
	TAIL_HEX,
	TAIL_OCT
};

static char *slurp( size_t *len) {
	size_t cap = 65536, n = 0, r;
	char *b = malloc( cap + 1);

	if ( b == NULL)
		exit( 1);
	while ( ( r = fread( b + n, 1, cap - n, stdin)) > 0) {
		n += r;
		if ( n == cap) {
			cap *= 2;
			b = realloc( b, cap + 1);
			if ( b == NULL)
				exit( 1);
		}
	}
	b[n] = '\0';
	*len = n;
	return b;
}

/* Index just past a // or block comment starting at i. */
static size_t skip_comment( const char *t, size_t n, size_t i) {
	if ( t[i + 1] == '/') {
		while ( i < n && t[i] != '\n')
			i++;
		return i;
	}
	i += 2;
	while ( i + 1 < n && !( t[i] == '*' && t[i + 1] == '/'))
		i++;
	return i + 2 <= n ? i + 2 : n;
}

/* Copy a character literal starting at i; returns the index past it. */
static size_t copy_char( const char *t, size_t n, size_t i, FILE *out) {
	fputc( t[i++], out);
	while ( i < n && t[i] != '\'' && t[i] != '\n') {
		if ( t[i] == '\\' && i + 1 < n)
			fputc( t[i++], out);
		fputc( t[i++], out);
	}
	if ( i < n)
		fputc( t[i++], out);
	return i;
}

/* ------------------------------------------------------------------ */
/* -1: join string literals                                           */

static void join_strings( const char *t, size_t n, FILE *out) {
	size_t i = 0;

	while ( i < n) {
		if ( strncmp( t + i, OFF_MARK, strlen( OFF_MARK)) == 0) {
			const char *e = strstr( t + i, ON_MARK);
			size_t end = e != NULL ? (size_t)( e - t) + strlen( ON_MARK) : n;

			fwrite( t + i, 1, end - i, out);
			i = end;
		} else if ( t[i] == '/' && ( t[i + 1] == '/' || t[i + 1] == '*')) {
			size_t e = skip_comment( t, n, i);

			fwrite( t + i, 1, e - i, out);
			i = e;
		} else if ( t[i] == '\'') {
			i = copy_char( t, n, i, out);
		} else if ( t[i] == '"') {
			enum tail tail = TAIL_NONE;

			fputc( t[i++], out);
			for ( ;;) {
				if ( i >= n || t[i] == '\n')
					break;
				if ( t[i] == '\\' && i + 1 < n) {
					size_t k = 0;

					fputc( t[i++], out);
					if ( t[i] == 'x') {
						fputc( t[i++], out);
						while ( i < n && isxdigit( (unsigned char)t[i]))
							fputc( t[i++], out);
						tail = TAIL_HEX;
					} else if ( t[i] >= '0' && t[i] <= '7') {
						while ( k < 3 && i < n && t[i] >= '0' && t[i] <= '7') {
							fputc( t[i++], out);
							k++;
						}
						tail = k < 3 ? TAIL_OCT : TAIL_NONE;
					} else {
						fputc( t[i++], out);
						tail = TAIL_NONE;
					}
					continue;
				}
				if ( t[i] == '"') {
					/* Look for another literal after blanks, at most one newline. */
					size_t j = i + 1;
					int nl = 0;
					char c;

					while ( j < n && ( t[j] == ' ' || t[j] == '\t' || ( t[j] == '\n' && !nl))) {
						if ( t[j] == '\n')
							nl = 1;
						j++;
					}
					if ( j < n && t[j] == '"') {
						c = t[j + 1];
						if ( !( tail == TAIL_HEX && isxdigit( (unsigned char)c)) && !( tail == TAIL_OCT && c >= '0' && c <= '7')) {
							i = j + 1;
							continue;
						}
					}
					fputc( t[i++], out);
					break;
				}
				fputc( t[i++], out);
				tail = TAIL_NONE;
			}
		} else
			fputc( t[i++], out);
	}
}

/* ------------------------------------------------------------------ */
/* -2: parenthesis spacing                                            */

static int is_ident( char c) {
	return isalnum( (unsigned char)c) || c == '_';
}

static int word_is( const char *w, size_t len, const char *s) {
	return strlen( s) == len && strncmp( w, s, len) == 0;
}

static int is_type_word( const char *w, size_t len) {
	static const char *words[] = { "void", "char", "short", "int", "long", "float", "double", "unsigned", "signed", "const", "volatile", NULL };
	size_t k;

	for ( k = 0; words[k] != NULL; k++)
		if ( word_is( w, len, words[k]))
			return 1;
	return len > 2 && w[len - 2] == '_' && w[len - 1] == 't';
}

static int is_keyword( const char *w, size_t len) {
	return word_is( w, len, "return") || word_is( w, len, "case") || word_is( w, len, "else") || word_is( w, len, "do") || word_is( w, len, "goto");
}

/* '(' at i and ')' at j with no nesting: is this a cast? */
static int is_cast( const char *t, size_t i, size_t j) {
	size_t k = i + 1, b;
	int words = 0, tag = 0;

	while ( k < j) {
		if ( t[k] == ' ' || t[k] == '\t' || t[k] == '*') {
			k++;
			continue;
		}
		if ( !is_ident( t[k]))
			return 0;
		b = k;
		while ( k < j && is_ident( t[k]))
			k++;
		if ( tag) {
			tag = 0; /* struct/union/enum tag: any name */
		} else if ( word_is( t + b, k - b, "struct") || word_is( t + b, k - b, "union") || word_is( t + b, k - b, "enum")) {
			tag = 1;
		} else if ( !is_type_word( t + b, k - b))
			return 0;
		words++;
	}
	if ( words == 0 || tag)
		return 0;

	/* Preceded by a name (not a keyword), ')' or ']': call, sizeof or declarator. */
	k = i;
	while ( k > 0 && ( t[k - 1] == ' ' || t[k - 1] == '\t'))
		k--;
	if ( k > 0 && ( t[k - 1] == ')' || t[k - 1] == ']'))
		return 0;
	if ( k > 0 && is_ident( t[k - 1])) {
		b = k;
		while ( b > 0 && is_ident( t[b - 1]))
			b--;
		if ( !is_keyword( t + b, k - b))
			return 0;
	}
	return 1;
}

/* ')' closing the '(' at i if nothing nests inside, else 0. */
static size_t simple_close( const char *t, size_t n, size_t i) {
	size_t j = i + 1;

	while ( j < n && strchr( "()\n\"';", t[j]) == NULL)
		j++;
	return j < n && t[j] == ')' ? j : 0;
}

static void paren_space( const char *t, size_t n, FILE *out) {
	size_t i = 0;

	while ( i < n) {
		if ( t[i] == '/' && ( t[i + 1] == '/' || t[i + 1] == '*')) {
			size_t e = skip_comment( t, n, i);

			fwrite( t + i, 1, e - i, out);
			i = e;
		} else if ( t[i] == '\'') {
			i = copy_char( t, n, i, out);
		} else if ( t[i] == '"') {
			fputc( t[i++], out);
			while ( i < n && t[i] != '"' && t[i] != '\n') {
				if ( t[i] == '\\' && i + 1 < n)
					fputc( t[i++], out);
				fputc( t[i++], out);
			}
			if ( i < n)
				fputc( t[i++], out);
		} else if ( t[i] == '(') {
			char c = i + 1 < n ? t[i + 1] : '\0';
			size_t j;

			fputc( t[i++], out);
			if ( c == ')' || c == ' ' || c == '\t' || c == '\n' || c == '\0')
				continue;
			j = simple_close( t, n, i - 1);
			if ( j != 0 && is_cast( t, i - 1, j)) {
				/* clang-format takes "(type)~x" for a binary operator */
				fwrite( t + i, 1, j + 1 - i, out);
				i = j + 1;
				if ( i + 1 < n && t[i] == ' ' && strchr( "~!-", t[i + 1]) != NULL)
					i++;
				continue;
			}
			fputc( ' ', out);
		} else
			fputc( t[i++], out);
	}
}

int main( int argc, char **argv) {
	size_t n;
	char *t;

	if ( argc != 2 || ( strcmp( argv[1], "-1") != 0 && strcmp( argv[1], "-2") != 0)) {
		fprintf( stderr, "usage: cstyle -1|-2 < in > out\n");
		return 2;
	}
	t = slurp( &n);
	if ( argv[1][1] == '1')
		join_strings( t, n, stdout);
	else
		paren_space( t, n, stdout);
	free( t);
	return fflush( stdout) == 0 ? 0 : 1;
}
