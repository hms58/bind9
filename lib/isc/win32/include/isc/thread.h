/*
 * Copyright (C) 1998-2000  Internet Software Consortium.
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

/* $Id: thread.h,v 1.10 2000/06/22 21:59:18 tale Exp $ */

#ifndef ISC_THREAD_H
#define ISC_THREAD_H 1

#include <windows.h>

#include <isc/lang.h>
#include <isc/result.h>

typedef HANDLE isc_thread_t;
typedef unsigned int isc_threadresult_t;
typedef void * isc_threadarg_t;
typedef isc_threadresult_t (WINAPI *isc_threadfunc_t)(isc_threadarg_t);

#define isc_thread_self (unsigned long)GetCurrentThreadId

ISC_LANG_BEGINDECLS

isc_result_t
isc_thread_create(isc_threadfunc_t, isc_threadarg_t, isc_thread_t *);

isc_result_t
isc_thread_join(isc_thread_t, isc_threadresult_t *);

ISC_LANG_ENDDECLS

#endif /* ISC_THREAD_H */