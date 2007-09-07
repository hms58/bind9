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

/* $Id: proforma.c,v 1.25 2000/06/21 22:45:11 tale Exp $ */

#ifndef RDATA_GENERIC_#_#_C
#define RDATA_GENERIC_#_#_C

#define RRTYPE_#_ATTRIBUTES (0)

static inline isc_result_t
fromtext_#(ARGS_FROMTEXT) {
	isc_token_t token;

	REQUIRE(type == #);
	REQUIRE(rdclass == #);

	RETERR(gettoken(lexer, &token, isc_tokentype_string, ISC_FALSE));

	return (ISC_R_NOTIMPLEMENTED);
}

static inline isc_result_t
totext_#(ARGS_TOTEXT) {

	REQUIRE(rdata->type == #);
	REQUIRE(rdata->rdclass == #);

	return (ISC_R_NOTIMPLEMENTED);
}

static inline isc_result_t
fromwire_#(ARGS_FROMWIRE) {

	REQUIRE(type == #);
	REQUIRE(rdclass == #);

	/* NONE or GLOBAL14 */
	dns_decompress_setmethods(dctx, DNS_COMPRESS_NONE);

	return (ISC_R_NOTIMPLEMENTED);
}

static inline isc_result_t
towire_#(ARGS_TOWIRE) {

	REQUIRE(rdata->type == #);
	REQUIRE(rdata->rdclass == #);

	/* NONE or GLOBAL14 */
	dns_compress_setmethods(cctx, DNS_COMPRESS_NONE);

	return (ISC_R_NOTIMPLEMENTED);
}

static inline int
compare_#(ARGS_COMPARE) {
	isc_region_t r1;
	isc_region_t r2;

	REQUIRE(rdata1->type == rdata2->type);
	REQUIRE(rdata1->rdclass == rdata2->rdclass);
	REQUIRE(rdata1->type == #);
	REQUIRE(rdata1->rdclass == #);

	dns_rdata_toregion(rdata1, &r1);
	dns_rdata_toregion(rdata2, &r2);
	return (compare_region(&r1, &r2));
}

static inline isc_result_t
fromstruct_#(ARGS_FROMSTRUCT) {
	dns_rdata_#_t *# = source;

	REQUIRE(type == #);
	REQUIRE(rdclass == #);
	REQUIRE(source != NULL);
	REQUIRE(#->common.rdtype == type);
	REQUIRE(#->common.rdclass == rdclass);

	return (ISC_R_NOTIMPLEMENTED);
}

static inline isc_result_t
tostruct_#(ARGS_TOSTRUCT) {

	REQUIRE(rdata->type == #);
	REQUIRE(rdata->rdclass == #);

	return (ISC_R_NOTIMPLEMENTED);
}

static inline void
freestruct_#(ARGS_FREESTRUCT) {
	dns_rdata_#_t *# = source;

	REQUIRE(source != NULL);
	REQUIRE(#->common.rdtype == #);
	REQUIRE(#->common.rdclass == #);
	
}

static inline isc_result_t
additionaldata_#(ARGS_ADDLDATA) {
	REQUIRE(rdata->type == #);
	REQUIRE(rdata->rdclass == #);

	(void)add;
	(void)arg;

	return (ISC_R_SUCCESS);
}

static inline isc_result_t
digest_#(ARGS_DIGEST) {
	isc_region_t r;

	REQUIRE(rdata->type == #);
	REQUIRE(rdata->rdclass == #);

	dns_rdata_toregion(rdata, &r);

	return ((digest)(arg, &r));
}

#endif	/* RDATA_GENERIC_#_#_C */