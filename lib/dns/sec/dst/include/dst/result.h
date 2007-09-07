/*
 * Copyright (C) 1999, 2000  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

/* $Id: result.h,v 1.15 2000/06/22 21:56:43 tale Exp $ */

#ifndef DST_RESULT_H
#define DST_RESULT_H 1

#include <isc/lang.h>
#include <isc/resultclass.h>

/*
 * Nothing in this file truly depends on <isc/result.h>, but the
 * DST result codes are considered to be publicly derived from
 * the ISC result codes, so including this file buys you the ISC_R_
 * namespace too.
 */
#include <isc/result.h>		/* Contractual promise. */

#define DST_R_UNSUPPORTEDALG		(ISC_RESULTCLASS_DST + 0)
/* 1 is unused */
/* 2 is unused */
#define DST_R_NULLKEY			(ISC_RESULTCLASS_DST + 3)
#define DST_R_INVALIDPUBLICKEY		(ISC_RESULTCLASS_DST + 4)
#define DST_R_INVALIDPRIVATEKEY		(ISC_RESULTCLASS_DST + 5)
/* 6 is unused */
#define DST_R_WRITEERROR		(ISC_RESULTCLASS_DST + 7)
#define DST_R_INVALIDPARAM		(ISC_RESULTCLASS_DST + 8)
/* 9 is unused */
/* 10 is unused */
#define DST_R_SIGNFAILURE		(ISC_RESULTCLASS_DST + 11)
/* 12 is unused */
/* 13 is unused */
#define DST_R_VERIFYFAILURE		(ISC_RESULTCLASS_DST + 14)
#define DST_R_NOTPUBLICKEY		(ISC_RESULTCLASS_DST + 15)
#define DST_R_NOTPRIVATEKEY		(ISC_RESULTCLASS_DST + 16)
#define DST_R_KEYCANNOTCOMPUTESECRET	(ISC_RESULTCLASS_DST + 17)
#define DST_R_COMPUTESECRETFAILURE	(ISC_RESULTCLASS_DST + 18)
#define DST_R_NORANDOMNESS		(ISC_RESULTCLASS_DST + 19)

#define DST_R_NRESULTS			20	/* Number of results */

ISC_LANG_BEGINDECLS

const char *
dst_result_totext(isc_result_t);

void
dst_result_register(void);

ISC_LANG_ENDDECLS

#endif /* DST_RESULT_H */