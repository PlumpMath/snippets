/* GNU fmt module -- simple text formatter.
   Copyright (C) 1994-2006 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* Originally written by Ross Paterson <rap@doc.ic.ac.uk>.
 * C module by Seong-Kook Shin <cinsky@gmail.com>. */
#ifndef fmt_h__
#define fmt_h__


/* Flags for fmt_new()
 *
 * FF_MALLOC_STR -- If provided, strings returned by fmt_vectorize()
 *                  will be allocated by malloc().  Otherwise, strings
 *                  are allocated from the obstack.
 *
 * FF_MALLOC_VEC -- If provided, the return value of fmt_vectorize()
 *                  will be allocated by malloc().  Otherwise, the
 *                  vector is allocated from the obstack.
 */
#define FF_MALLOC_STR   0x01
#define FF_MALLOC_VEC   0x02

struct fmt_;
typedef struct fmt_ fmt_t;

/*
 * Create FMT_T struct for the simple text formatting.
 *
 * FLAGS - one or more FF_* macros
 *
 * fmt_new() returns a pointer to new FMT_T struct on success,
 * otherwise it returns NULL.
 */
extern fmt_t *fmt_new(unsigned flags);

/*
 * Deallocate all resources occupied by FMT.
 *
 * Note that string vectors returned by fmt_vectorize() are not freed
 * automatically if FF_MALLOC was used.
 */
extern void fmt_delete(fmt_t *fmt);

/*
 * Set the maximum line width.
 */
extern void fmt_set_width(fmt_t *p, int width);

/*
 * Format the string S so that it fits in the maximum width.
 *
 * fmt_format() returns a pointer to the formatted string.  The
 * returned string is valid until next call to fmt_format().  You can
 * force fmt_format() to deallocate the previous returned string by
 * passing NULL in S if any.
 *
 * Note that if S is NULL, any resources allocated by fmt_vectorize()
 * from the obstack are freed.
 */
extern char *fmt_format (fmt_t *f, const char *s);

/*
 * Make an array of pointers to strings from the string obtained
 * by previous call to fmt_format().
 *
 * Memory used by fmt_vectorize() will be governed by FLAGS provided
 * in fmt_new().
 *
 * Note that any resources that is not allocated by malloc() in this
 * function will be freed on next call of fmt_format().
 */
char **fmt_vectorize(fmt_t *f);

#endif  /* fmt_h__ */
