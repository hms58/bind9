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

/* $Id: sockaddr.h,v 1.30 2000/06/22 21:58:02 tale Exp $ */

#ifndef ISC_SOCKADDR_H
#define ISC_SOCKADDR_H 1

#include <isc/lang.h>
#include <isc/net.h>
#include <isc/types.h>

struct isc_sockaddr {
	union {
		struct sockaddr		sa;
		struct sockaddr_in	sin;
		struct sockaddr_in6	sin6;
	}				type;
	unsigned int			length;		/* XXXRTH beginning? */
	ISC_LINK(struct isc_sockaddr)	link;
};

typedef ISC_LIST(struct isc_sockaddr)	isc_sockaddrlist_t;

ISC_LANG_BEGINDECLS

isc_boolean_t
isc_sockaddr_equal(const isc_sockaddr_t *a, const isc_sockaddr_t *b);

isc_boolean_t
isc_sockaddr_eqaddr(const isc_sockaddr_t *a, const isc_sockaddr_t *b);

isc_boolean_t
isc_sockaddr_eqaddrprefix(const isc_sockaddr_t *a, const isc_sockaddr_t *b,
			  unsigned int prefixlen);

unsigned int
isc_sockaddr_hash(const isc_sockaddr_t *sockaddr, isc_boolean_t address_only);

void
isc_sockaddr_any(isc_sockaddr_t *sockaddr);

void
isc_sockaddr_any6(isc_sockaddr_t *sockaddr);

void
isc_sockaddr_fromin(isc_sockaddr_t *sockaddr, const struct in_addr *ina,
		    in_port_t port);

void
isc_sockaddr_fromin6(isc_sockaddr_t *sockaddr, const struct in6_addr *ina6,
		     in_port_t port);

void
isc_sockaddr_v6fromin(isc_sockaddr_t *sockaddr, const struct in_addr *ina,
		      in_port_t port);

void
isc_sockaddr_fromnetaddr(isc_sockaddr_t *sockaddr, const isc_netaddr_t *na,
			 in_port_t port);

int
isc_sockaddr_pf(const isc_sockaddr_t *sockaddr);
/*
 * Get the protocol family of 'sockaddr'.
 *
 * Requires:
 *
 *	'sockaddr' is a valid sockaddr with an address family of AF_INET
 *	or AF_INET6.
 *
 * Returns:
 *
 *	The protocol family of 'sockaddr', e.g. PF_INET or PF_INET6.
 */

void
isc_sockaddr_setport(isc_sockaddr_t *sockaddr, in_port_t port);
/*
 * Set the port of 'sockaddr' to 'port'.
 */

in_port_t
isc_sockaddr_getport(isc_sockaddr_t *sockaddr);
/*
 * Get the port stored in 'sockaddr'.
 */

isc_result_t
isc_sockaddr_totext(const isc_sockaddr_t *sockaddr, isc_buffer_t *target);
/*
 * Append a text representation of 'sockaddr' to the buffer 'target'.
 * The text will include both the IP address (v4 or v6) and the port.
 * The text is null terminated, but the terminating null is not
 * part of the buffer's used region.
 *
 * Returns:
 *	ISC_R_SUCCESS
 *	ISC_R_NOSPACE	The text or the null termination did not fit.
 */

void
isc_sockaddr_format(isc_sockaddr_t *sa, char *array, unsigned int size);
/*
 * Format a human-readable representation of the socket address '*sa'
 * into the character array 'array', which is of size 'size'.
 * The resulting string is guaranteed to be null-terminated.
 */

isc_boolean_t
isc_sockaddr_ismulticast(isc_sockaddr_t *sa);
/*
 * Returns ISC_TRUE if the address is a multicast address
 */

#define ISC_SOCKADDR_FORMATSIZE \
	sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:XXX.XXX.XXX.XXX#YYYYY")
/*
 * Minimum size of array to pass to isc_sockaddr_format().
 */

ISC_LANG_ENDDECLS

#endif /* ISC_SOCKADDR_H */