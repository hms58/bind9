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

/* $Id: log.h,v 1.21 2000/06/23 01:51:18 bwelling Exp $ */

/* Principal Authors: DCL */

#ifndef DNS_LOG_H
#define DNS_LOG_H 1

#include <isc/lang.h>
#include <isc/log.h>

extern isc_log_t *dns_lctx;
extern isc_logcategory_t dns_categories[];
extern isc_logmodule_t dns_modules[];

#define DNS_LOGCATEGORY_NOTIFY		(&dns_categories[0])
#define DNS_LOGCATEGORY_DATABASE	(&dns_categories[1])
#define DNS_LOGCATEGORY_SECURITY	(&dns_categories[2])
#define DNS_LOGCATEGORY_CONFIG		(&dns_categories[3])
#define DNS_LOGCATEGORY_DNSSEC		(&dns_categories[4])
#define DNS_LOGCATEGORY_RESOLVER	(&dns_categories[5])
#define DNS_LOGCATEGORY_XFER_IN		(&dns_categories[6])
#define DNS_LOGCATEGORY_XFER_OUT	(&dns_categories[7])
#define DNS_LOGCATEGORY_DISPATCH	(&dns_categories[8])

/* Backwards compatibility. */
#define DNS_LOGCATEGORY_GENERAL		ISC_LOGCATEGORY_GENERAL

#define DNS_LOGMODULE_DB		(&dns_modules[0])
#define DNS_LOGMODULE_RBTDB		(&dns_modules[1])
#define DNS_LOGMODULE_RBTDB64		(&dns_modules[2])
#define DNS_LOGMODULE_RBT		(&dns_modules[3])
#define DNS_LOGMODULE_RDATA		(&dns_modules[4])
#define DNS_LOGMODULE_MASTER		(&dns_modules[5])
#define DNS_LOGMODULE_MESSAGE		(&dns_modules[6])
#define DNS_LOGMODULE_CACHE		(&dns_modules[7])
#define DNS_LOGMODULE_CONFIG		(&dns_modules[8])
#define DNS_LOGMODULE_RESOLVER		(&dns_modules[9])
#define DNS_LOGMODULE_ZONE		(&dns_modules[10])
#define DNS_LOGMODULE_JOURNAL		(&dns_modules[11])
#define DNS_LOGMODULE_ADB		(&dns_modules[12])
#define DNS_LOGMODULE_XFER_IN		(&dns_modules[13])
#define DNS_LOGMODULE_XFER_OUT		(&dns_modules[14])
#define DNS_LOGMODULE_ACL		(&dns_modules[15])
#define DNS_LOGMODULE_VALIDATOR		(&dns_modules[16])
#define DNS_LOGMODULE_DISPATCH		(&dns_modules[17])
#define DNS_LOGMODULE_REQUEST		(&dns_modules[18])
#define DNS_LOGMODULE_MASTERDUMP	(&dns_modules[19])
#define DNS_LOGMODULE_TSIG		(&dns_modules[20])
#define DNS_LOGMODULE_TKEY		(&dns_modules[21])

ISC_LANG_BEGINDECLS

void
dns_log_init(isc_log_t *lctx);
/*
 * Make the libdns categories and modules available for use with the
 * ISC logging library.
 *
 * Requires:
 *	lctx is a valid logging context.
 *
 *	dns_log_init() is called only once.
 *
 * Ensures:
 * 	The catgories and modules defined above are available for
 * 	use by isc_log_usechannnel() and isc_log_write().
 */

void
dns_log_setcontext(isc_log_t *lctx);
/*
 * Make the libdns library use the provided context for logging internal
 * messages.
 *
 * Requires:
 *	lctx is a valid logging context.
 *
 *	dns_log_setcontext() is called only once.
 */

ISC_LANG_ENDDECLS

#endif /* DNS_LOG_H */