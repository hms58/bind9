/*
 * Copyright (C) 1998, 1999, 2000  Internet Software Consortium.
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

#ifndef DNS_TYPES_H
#define DNS_TYPES_H 1

/*
 * Including this file gives you type declarations suitable for use in
 * .h files, which lets us avoid circular type reference problems.
 *
 * To actually use a type or get declarations of its methods, you must
 * include the appropriate .h file too.
 */

#include <isc/types.h>

typedef struct dns_a6context			dns_a6context_t;
typedef struct dns_acl 				dns_acl_t;
typedef struct dns_aclelement 			dns_aclelement_t;
typedef struct dns_aclenv			dns_aclenv_t;
typedef struct dns_adb				dns_adb_t;
typedef struct dns_adbaddrinfo			dns_adbaddrinfo_t;
typedef ISC_LIST(dns_adbaddrinfo_t)		dns_adbaddrinfolist_t;
typedef struct dns_adbentry			dns_adbentry_t;
typedef struct dns_adbfind			dns_adbfind_t;
typedef ISC_LIST(dns_adbfind_t)			dns_adbfindlist_t;
typedef struct dns_byaddr			dns_byaddr_t;
typedef struct dns_cache			dns_cache_t;
typedef isc_uint16_t				dns_cert_t;
typedef struct dns_compress			dns_compress_t;
typedef struct dns_db				dns_db_t;
typedef struct dns_dbiterator			dns_dbiterator_t;
typedef void					dns_dbload_t;
typedef void					dns_dbnode_t;
typedef struct dns_dbtable			dns_dbtable_t;
typedef void					dns_dbversion_t;
typedef struct dns_decompress			dns_decompress_t;
typedef struct dns_dispatch			dns_dispatch_t;
typedef struct dns_dispatchevent		dns_dispatchevent_t;
typedef struct dns_dispatchlist			dns_dispatchlist_t;
typedef struct dns_dispatchmgr			dns_dispatchmgr_t;
typedef struct dns_dispentry			dns_dispentry_t;
typedef struct dns_fetch			dns_fetch_t;
typedef struct dns_fixedname			dns_fixedname_t;
typedef struct dns_forwarders			dns_forwarders_t;
typedef isc_uint16_t				dns_keyflags_t;
typedef struct dns_keynode			dns_keynode_t;
typedef struct dns_keytable			dns_keytable_t;
typedef isc_uint16_t				dns_keytag_t;
typedef struct dns_message			dns_message_t;
typedef isc_uint16_t				dns_messageid_t;
typedef isc_region_t				dns_label_t;
typedef struct dns_name				dns_name_t;
typedef ISC_LIST(dns_name_t)			dns_namelist_t;
typedef isc_uint16_t				dns_opcode_t;
typedef unsigned char				dns_offsets_t[128];
typedef struct dns_peer				dns_peer_t;
typedef struct dns_peerlist			dns_peerlist_t;
typedef struct dns_rbt				dns_rbt_t;
typedef isc_uint16_t				dns_rcode_t;
typedef struct dns_rdata			dns_rdata_t;
typedef struct dns_rdatacallbacks		dns_rdatacallbacks_t;
typedef isc_uint16_t				dns_rdataclass_t;
typedef struct dns_rdatalist			dns_rdatalist_t;
typedef struct dns_rdataset			dns_rdataset_t;
typedef ISC_LIST(dns_rdataset_t)		dns_rdatasetlist_t;
typedef struct dns_rdatasetiter			dns_rdatasetiter_t;
typedef isc_uint16_t				dns_rdatatype_t;
typedef struct dns_request			dns_request_t;
typedef struct dns_requestmgr			dns_requestmgr_t;
typedef struct dns_resolver			dns_resolver_t;
typedef isc_uint8_t				dns_secalg_t;
typedef isc_uint8_t				dns_secproto_t;
typedef struct dns_signature			dns_signature_t;
typedef struct dns_ssurule			dns_ssurule_t;
typedef struct dns_ssutable			dns_ssutable_t;
typedef struct dns_tkey_ctx			dns_tkey_ctx_t;
typedef isc_uint16_t				dns_trust_t;
typedef struct dns_tsig_keyring			dns_tsig_keyring_t;
typedef struct dns_tsigkey			dns_tsigkey_t;
typedef isc_uint32_t				dns_ttl_t;
typedef struct dns_validator			dns_validator_t;
typedef struct dns_view				dns_view_t;
typedef ISC_LIST(dns_view_t)			dns_viewlist_t;
typedef struct dns_zone				dns_zone_t;
typedef ISC_LIST(dns_zone_t)			dns_zonelist_t;
typedef struct dns_zonemgr			dns_zonemgr_t;
typedef struct dns_zt				dns_zt_t;

typedef enum {
	dns_bitlabel_0 = 0,
	dns_bitlabel_1 = 1
} dns_bitlabel_t;

typedef enum {
	dns_fwdpolicy_none = 0,
	dns_fwdpolicy_first = 1,
	dns_fwdpolicy_only = 2
} dns_fwdpolicy_t;

typedef enum {
	dns_labeltype_ordinary = 0,
	dns_labeltype_bitstring = 1
} dns_labeltype_t;

typedef enum {
	dns_namereln_none = 0,
	dns_namereln_contains = 1,
	dns_namereln_subdomain = 2,
	dns_namereln_equal = 3,
	dns_namereln_commonancestor = 4
} dns_namereln_t;

typedef enum {
	dns_one_answer, dns_many_answers
} dns_transfer_format_t;

#include <dns/enumtype.h>

enum {
	dns_rdatatype_none = 0,
	DNS_TYPEENUM
	dns_rdatatype_ixfr = 251,
	dns_rdatatype_axfr = 252,
	dns_rdatatype_mailb = 253,
	dns_rdatatype_maila = 254,
	dns_rdatatype_any = 255
};

#include <dns/enumclass.h>
enum {
	DNS_CLASSENUM
	dns_rdataclass_ch = 3,
	dns_rdataclass_none = 254	/* RFC2136 */
};

/*
 * rcodes.
 */
enum {
	/*
	 * Standard rcodes.
	 */
	dns_rcode_noerror = 0,
	dns_rcode_formerr = 1,
	dns_rcode_servfail = 2,
	dns_rcode_nxdomain = 3,
	dns_rcode_notimp = 4,
	dns_rcode_refused = 5,
	dns_rcode_yxdomain = 6,
	dns_rcode_yxrrset = 7,
	dns_rcode_nxrrset = 8,
	dns_rcode_notauth = 9,
	dns_rcode_notzone = 10,
	/*
	 * Extended rcodes.
	 */
	dns_rcode_badvers = 16
};

/*
 * TSIG errors.
 */
enum {
	dns_tsigerror_badsig = 16,
	dns_tsigerror_badkey = 17,
	dns_tsigerror_badtime = 18,
	dns_tsigerror_badmode = 19,
	dns_tsigerror_badname = 20,
	dns_tsigerror_badalg = 21
};

/*
 * Opcodes.
 */
enum {
	dns_opcode_query = 0,
	dns_opcode_iquery = 1,
	dns_opcode_status = 2,
	dns_opcode_notify = 4,
	dns_opcode_update = 5		/* dynamic update */
};

/*
 * Trust levels.
 */
enum {
	dns_trust_none = 0,
	dns_trust_pending = 1,
	dns_trust_additional = 2,
	dns_trust_glue = 3,
	dns_trust_answer = 4,
	dns_trust_authauthority = 5,
	dns_trust_authanswer = 6,
	dns_trust_secure = 7,
	dns_trust_authsecure = 8,
	dns_trust_ultimate = 9
};

/*
 * Name checking severites.
 */
typedef enum {
	dns_severity_ignore,
	dns_severity_warn,
	dns_severity_fail
} dns_severity_t;

/*
 * Functions.
 */
typedef isc_result_t
(*dns_addrdatasetfunc_t)(void *, dns_name_t *, dns_rdataset_t *);

typedef isc_result_t
(*dns_additionaldatafunc_t)(void *, dns_name_t *, dns_rdatatype_t);

typedef isc_result_t
(*dns_digestfunc_t)(void *, isc_region_t *);

typedef void
(*dns_xfrindone_t)(dns_zone_t *, isc_result_t);

#endif /* DNS_TYPES_H */
