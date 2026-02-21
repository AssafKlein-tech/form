/** @file comtool.h
 *
 *  Utility routines for the compiler.
 */
/* #[ License : */
/*
 *   Copyright (C) 1984-2026 J.A.M. Vermaseren
 *   When using this file you are requested to refer to the publication
 *   J.A.M.Vermaseren "New features of FORM" math-ph/0010025
 *   This is considered a matter of courtesy as the development was paid
 *   for by FOM the Dutch physics granting agency and we would like to
 *   be able to track its scientific use to convince FOM of its value
 *   for the community.
 *
 *   This file is part of FORM.
 *
 *   FORM is free software: you can redistribute it and/or modify it under the
 *   terms of the GNU General Public License as published by the Free Software
 *   Foundation, either version 3 of the License, or (at your option) any later
 *   version.
 *
 *   FORM is distributed in the hope that it will be useful, but WITHOUT ANY
 *   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *   FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *   details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with FORM.  If not, see <http://www.gnu.org/licenses/>.
 */
/* #] License : */ 
#ifndef FORM_COMTOOL_H_
#define FORM_COMTOOL_H_
/*
  	#[ Includes :
*/

#include "form3.h"

/*
  	#] Includes : 
  	#[ Inline functions :
*/

/**
 * Skips over whitespace characters in the buffer, including commas,
 * which are treated as whitespace characters in the FORM compiler.
 *
 * @note To avoid skipping commas, use `SKIPBLANKS(s)` instead.
 *
 * @param[in,out]  s  Pointer to the current position in the buffer. The buffer
 *                    must be null-terminated. On return, the pointer is
 *                    advanced to the first non-whitespace character, or the
 *                    null terminator if none is found.
 */
static inline void SkipSpaces(UBYTE **s)
{
	const char *p = (const char *)*s;
	while ( *p == ' ' || *p == ',' || *p == '\t' ) p++;
	*s = (UBYTE *)p;
}

/**
 * Checks whether the next token in the buffer is the given keyword,
 * ignoring case. If found, the keyword is consumed and the pointer is advanced
 * to the first non-whitespace character following the keyword.
 *
 * @param[in,out]  s    Pointer to the current position in the buffer.
 * 						The buffer must be null-terminated. On return,
 * 						the pointer is advanced if the keyword is found.
 * @param          opt  Case-insensitive keyword.
 * @return              1 if the keyword is found, otherwise 0.
 */
static inline int ConsumeOption(UBYTE **s, const char *opt)
{
	const char *p = (const char *)*s;
	while ( *p && *opt && tolower(*p) == tolower(*opt) ) {
		p++;
		opt++;
	}
	/* Check if `opt` ended. */
	if ( !*opt ) {
		/* Check if `*p` is a word boundary. */
		UINT c = FG.cTable[(unsigned char)*p];
		if ( c != 0 && c != 1 && *p != '_' && *p != '$' ) {
			/* Consume the option. Skip the trailing whitespace. */
			*s = (UBYTE *)p;
			SkipSpaces(s);
			return(1);
		}
	}
	return(0);
}

/*
  	#] Inline functions : 
*/
#endif  /* FORM_COMTOOL_H_ */
