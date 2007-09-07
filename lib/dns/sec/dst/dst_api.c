/*
 * Portions Copyright (C) 1999, 2000  Internet Software Consortium.
 * Portions Copyright (C) 1995-2000 by Network Associates, Inc.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM AND
 * NETWORK ASSOCIATES DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE CONSORTIUM OR NETWORK
 * ASSOCIATES BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
 * USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Principal Author: Brian Wellington
 * $Id: dst_api.c,v 1.56 2000/06/12 18:05:10 bwelling Exp $
 */

#include <config.h>

#include <stdlib.h>

#include <isc/buffer.h>
#include <isc/dir.h>
#include <isc/entropy.h>
#include <isc/lex.h>
#include <isc/mem.h>
#include <isc/once.h>
#include <isc/random.h>
#include <isc/string.h>
#include <isc/time.h>
#include <isc/util.h>

#include <dns/fixedname.h>
#include <dns/name.h>
#include <dns/rdata.h>
#include <dns/types.h>
#include <dns/keyvalues.h>

#include <dst/result.h>

#include "dst_internal.h"

#include <openssl/rand.h>

#define KEY_MAGIC	0x4453544BU	/* DSTK */
#define CTX_MAGIC	0x44535443U	/* DSTC */

#define VALID_KEY(x) ISC_MAGIC_VALID(x, KEY_MAGIC)
#define VALID_CTX(x) ISC_MAGIC_VALID(x, CTX_MAGIC)

static dst_func_t *dst_t_func[DST_MAX_ALGS];
static isc_mem_t *dst_memory_pool = NULL;
static isc_entropy_t *dst_entropy_pool = NULL;
static unsigned int dst_entropy_flags = 0;
static isc_boolean_t dst_initialized = ISC_FALSE;

/*
 * Static functions.
 */
static dst_key_t *	get_key_struct(dns_name_t *name,
				       const unsigned int alg,
				       const unsigned int flags,
				       const unsigned int protocol,
				       const unsigned int bits,
				       isc_mem_t *mctx);
static isc_result_t	read_public_key(const char *filename,
					isc_mem_t *mctx,
					dst_key_t **keyp);
static isc_result_t	write_public_key(const dst_key_t *key,
					 const char *directory);
static isc_result_t	buildfilename(dns_name_t *name,
				      const unsigned int id, 
				      const unsigned int alg,
				      const unsigned int type,
				      const char *directory,
				      isc_buffer_t *out);

#define RETERR(x) do { \
	result = (x); \
	if (result != ISC_R_SUCCESS) \
		goto out; \
	} while (0)

isc_result_t
dst_lib_init(isc_mem_t *mctx, isc_entropy_t *ectx, unsigned int eflags) {
	isc_result_t result;

	REQUIRE(mctx != NULL && ectx != NULL);
	REQUIRE(dst_initialized == ISC_FALSE);

	isc_mem_attach(mctx, &dst_memory_pool);
	isc_entropy_attach(ectx, &dst_entropy_pool);
	dst_entropy_flags = eflags;

	dst_result_register();

	memset(dst_t_func, 0, sizeof(dst_t_func));
	RETERR(dst__hmacmd5_init(&dst_t_func[DST_ALG_HMACMD5]));
#ifdef DNSSAFE
	RETERR(dst__dnssafersa_init(&dst_t_func[DST_ALG_RSA]));
#endif
#ifdef OPENSSL
	RETERR(dst__openssl_init());
	RETERR(dst__openssldsa_init(&dst_t_func[DST_ALG_DSA]));
	RETERR(dst__openssldh_init(&dst_t_func[DST_ALG_DH]));
#endif

	dst_initialized = ISC_TRUE;
	return (ISC_R_SUCCESS);

 out:
	dst_lib_destroy();
	return (result);
}

void
dst_lib_destroy(void) {
	RUNTIME_CHECK(dst_initialized == ISC_TRUE);
	dst_initialized = ISC_FALSE;

	dst__hmacmd5_destroy();
#ifdef DNSSAFE
	dst__dnssafersa_destroy();
#endif
#ifdef OPENSSL
	dst__openssldsa_destroy();
	dst__openssldh_destroy();
	dst__openssl_destroy();
#endif
	if (dst_memory_pool != NULL)
		isc_mem_detach(&dst_memory_pool);
	if (dst_entropy_pool != NULL)
		isc_entropy_detach(&dst_entropy_pool);

}

isc_boolean_t
dst_algorithm_supported(const unsigned int alg) {
	REQUIRE(dst_initialized == ISC_TRUE);

	if (alg >= DST_MAX_ALGS || dst_t_func[alg] == NULL)
		return (ISC_FALSE);
	return (ISC_TRUE);
}

isc_result_t
dst_context_create(dst_key_t *key, isc_mem_t *mctx, dst_context_t **dctxp) {
	dst_context_t *dctx;
	isc_result_t result;

	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(VALID_KEY(key));
	REQUIRE(mctx != NULL);
	REQUIRE(dctxp != NULL && *dctxp == NULL);

	if (key->func->createctx == NULL)
		return (DST_R_UNSUPPORTEDALG);
	if (key->opaque == NULL)
		return (DST_R_NULLKEY);

	dctx = isc_mem_get(mctx, sizeof(dst_context_t));
	if (dctx == NULL)
		return (ISC_R_NOMEMORY);
	dctx->key = key;
	dctx->mctx = mctx;
	result = key->func->createctx(key, dctx);
	if (result != ISC_R_SUCCESS) {
		isc_mem_put(mctx, dctx, sizeof(dst_context_t));
		return (result);
	}
	dctx->magic = CTX_MAGIC;
	*dctxp = dctx;
	return (ISC_R_SUCCESS);
}

void
dst_context_destroy(dst_context_t **dctxp) {
	dst_context_t *dctx;

	REQUIRE(dctxp != NULL && VALID_CTX(*dctxp));

	dctx = *dctxp;
	INSIST(dctx->key->func->destroyctx != NULL);
	dctx->key->func->destroyctx(dctx);
	dctx->magic = 0;
	isc_mem_put(dctx->mctx, dctx, sizeof(dst_context_t));
	*dctxp = NULL;
}

isc_result_t
dst_context_adddata(dst_context_t *dctx, const isc_region_t *data) {
	REQUIRE(VALID_CTX(dctx));
	REQUIRE(data != NULL);
	INSIST(dctx->key->func->adddata != NULL);

	return (dctx->key->func->adddata(dctx, data));
}

isc_result_t
dst_context_sign(dst_context_t *dctx, isc_buffer_t *sig) {
	REQUIRE(VALID_CTX(dctx));
	REQUIRE(sig != NULL);

	if (dst_algorithm_supported(dctx->key->key_alg) == ISC_FALSE)
		return (DST_R_UNSUPPORTEDALG);
	if (dctx->key->opaque == NULL)
		return (DST_R_NULLKEY);
	if (dctx->key->func->sign == NULL)
		return (DST_R_NOTPRIVATEKEY);

	return (dctx->key->func->sign(dctx, sig));
}

isc_result_t
dst_context_verify(dst_context_t *dctx, isc_region_t *sig) {
	REQUIRE(VALID_CTX(dctx));
	REQUIRE(sig != NULL);

	if (dst_algorithm_supported(dctx->key->key_alg) == ISC_FALSE)
		return (DST_R_UNSUPPORTEDALG);
	if (dctx->key->opaque == NULL)
		return (DST_R_NULLKEY);
	if (dctx->key->func->verify == NULL)
		return (DST_R_NOTPUBLICKEY);

	return (dctx->key->func->verify(dctx, sig));
}

isc_result_t
dst_key_computesecret(const dst_key_t *pub, const dst_key_t *priv,
		  isc_buffer_t *secret) 
{
	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(VALID_KEY(pub) && VALID_KEY(priv));
	REQUIRE(secret != NULL);

	if (dst_algorithm_supported(pub->key_alg)  == ISC_FALSE ||
	    dst_algorithm_supported(priv->key_alg) == ISC_FALSE)
		return (DST_R_UNSUPPORTEDALG);

	if (pub->opaque == NULL || priv->opaque == NULL)
		return (DST_R_NULLKEY);

	if (pub->key_alg != priv->key_alg ||
	    pub->func->computesecret == NULL ||
	    priv->func->computesecret == NULL)
		return (DST_R_KEYCANNOTCOMPUTESECRET);

	if (dst_key_isprivate(priv) == ISC_FALSE)
		return (DST_R_NOTPRIVATEKEY);

	return (pub->func->computesecret(pub, priv, secret));
}

isc_result_t 
dst_key_tofile(const dst_key_t *key, const int type, const char *directory) {
	isc_result_t ret = ISC_R_SUCCESS;

	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(VALID_KEY(key));
	REQUIRE((type & (DST_TYPE_PRIVATE | DST_TYPE_PUBLIC)) != 0);

	if (dst_algorithm_supported(key->key_alg) == ISC_FALSE)
		return (DST_R_UNSUPPORTEDALG);

	if (key->func->tofile == NULL)
		return (DST_R_UNSUPPORTEDALG);

	if (type & DST_TYPE_PUBLIC) {
		ret = write_public_key(key, directory);
		if (ret != ISC_R_SUCCESS)
			return (ret);
	}

	if ((type & DST_TYPE_PRIVATE) &&
	    (key->key_flags & DNS_KEYFLAG_TYPEMASK) != DNS_KEYTYPE_NOKEY)
		return (key->func->tofile(key, directory));
	else
		return (ISC_R_SUCCESS);
}

isc_result_t
dst_key_fromfile(dns_name_t *name, const isc_uint16_t id,
		 const unsigned int alg, const int type, const char *directory,
		 isc_mem_t *mctx, dst_key_t **keyp)
{
	char filename[ISC_DIR_NAMEMAX];
	isc_buffer_t b;
	dst_key_t *key;
	isc_result_t result;

	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(dns_name_isabsolute(name));
	REQUIRE((type & (DST_TYPE_PRIVATE | DST_TYPE_PUBLIC)) != 0);
	REQUIRE(mctx != NULL);
	REQUIRE(keyp != NULL && *keyp == NULL);

	if (dst_algorithm_supported(alg) == ISC_FALSE)
		return (DST_R_UNSUPPORTEDALG);

	isc_buffer_init(&b, filename, sizeof filename);
	result = buildfilename(name, id, alg, type, directory, &b);
	if (result != ISC_R_SUCCESS)
		return (result);

	key = NULL;
	result = dst_key_fromnamedfile(filename, type, mctx, &key);
	if (result != ISC_R_SUCCESS)
		return (result);

	if (!dns_name_equal(name, key->key_name) ||
	    id != key->key_id ||
	    alg != key->key_alg)
	{
		dst_key_free(&key);
		return (DST_R_INVALIDPUBLICKEY);
	}
	*keyp = key;
	return (ISC_R_SUCCESS);
}

isc_result_t
dst_key_fromnamedfile(const char *filename, const int type, isc_mem_t *mctx,
		      dst_key_t **keyp)
{
	isc_result_t result;
	dst_key_t *pubkey = NULL, *key = NULL;
	isc_uint16_t id;

	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(filename != NULL);
	REQUIRE((type & (DST_TYPE_PRIVATE | DST_TYPE_PUBLIC)) != 0);
	REQUIRE(mctx != NULL);
	REQUIRE(keyp != NULL && *keyp == NULL);

	result = read_public_key(filename, mctx, &pubkey);

	if (result == ISC_R_NOTFOUND)
		return (DST_R_INVALIDPUBLICKEY);
	else if (result != ISC_R_SUCCESS)
		return (result);

	if (type == DST_TYPE_PUBLIC ||
	    (pubkey->key_flags & DNS_KEYFLAG_TYPEMASK) == DNS_KEYTYPE_NOKEY)
	{
		*keyp = pubkey;
		return (ISC_R_SUCCESS);
	}
	
	key = get_key_struct(pubkey->key_name, pubkey->key_alg,
			     pubkey->key_flags, pubkey->key_proto, 0, mctx);
	id = pubkey->key_id;
	dst_key_free(&pubkey);

	if (key == NULL)
		return (ISC_R_NOMEMORY);

	if (key->func->fromfile == NULL) {
		dst_key_free(&key);
		return (DST_R_UNSUPPORTEDALG);
	}

	result = key->func->fromfile(key, id, filename);
	if (result != ISC_R_SUCCESS) {
		dst_key_free(&key);
		return (result);
	}

	*keyp = key;
	return (ISC_R_SUCCESS);
}

isc_result_t
dst_key_todns(const dst_key_t *key, isc_buffer_t *target) {
	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(VALID_KEY(key));
	REQUIRE(target != NULL);

	if (dst_algorithm_supported(key->key_alg) == ISC_FALSE)
		return (DST_R_UNSUPPORTEDALG);

	if (key->func->todns == NULL)
		return (DST_R_UNSUPPORTEDALG);

	if (isc_buffer_availablelength(target) < 4)
		return (ISC_R_NOSPACE);
	isc_buffer_putuint16(target, (isc_uint16_t)(key->key_flags & 0xffff));
	isc_buffer_putuint8(target, (isc_uint8_t)key->key_proto);
	isc_buffer_putuint8(target, (isc_uint8_t)key->key_alg);

	if (key->key_flags & DNS_KEYFLAG_EXTENDED) {
		if (isc_buffer_availablelength(target) < 2)
			return (ISC_R_NOSPACE);
		isc_buffer_putuint16(target,
				     (isc_uint16_t)((key->key_flags >> 16)
						    & 0xffff));
	}

	if (key->opaque == NULL) /* NULL KEY */
		return (ISC_R_SUCCESS);

	return (key->func->todns(key, target));
}

isc_result_t
dst_key_fromdns(dns_name_t *name, isc_buffer_t *source, isc_mem_t *mctx,
		dst_key_t **keyp)
{
	isc_uint8_t alg, proto;
	isc_uint32_t flags, extflags;

	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(dns_name_isabsolute(name));
	REQUIRE(source != NULL);
	REQUIRE(mctx != NULL);
	REQUIRE(keyp != NULL && *keyp == NULL);

	if (isc_buffer_remaininglength(source) < 4)
		return (DST_R_INVALIDPUBLICKEY);
	flags = isc_buffer_getuint16(source);
	proto = isc_buffer_getuint8(source);
	alg = isc_buffer_getuint8(source);

	if (!dst_algorithm_supported(alg))
		return (DST_R_UNSUPPORTEDALG);

	if (flags & DNS_KEYFLAG_EXTENDED) {
		if (isc_buffer_remaininglength(source) < 2)
			return (DST_R_INVALIDPUBLICKEY);
		extflags = isc_buffer_getuint16(source);
		flags |= (extflags << 16);
	}

	return (dst_key_frombuffer(name, alg, flags, proto, source, mctx,
				   keyp));
}

isc_result_t
dst_key_frombuffer(dns_name_t *name, const unsigned int alg,
		   const unsigned int flags, const unsigned int protocol,
		   isc_buffer_t *source, isc_mem_t *mctx, dst_key_t **keyp)
{
	dst_key_t *key;
	isc_result_t ret;

	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(dns_name_isabsolute(name));
	REQUIRE(source != NULL);
	REQUIRE(mctx != NULL);
	REQUIRE(keyp != NULL && *keyp == NULL);

	if (dst_algorithm_supported(alg) == ISC_FALSE)
		return (DST_R_UNSUPPORTEDALG);

	key = get_key_struct(name, alg, flags, protocol, 0, mctx);
	if (key == NULL)
		return (ISC_R_NOMEMORY);

	if (key->func->fromdns == NULL) {
		dst_key_free(&key);
		return (DST_R_UNSUPPORTEDALG);
	}

	ret = key->func->fromdns(key, source);
	if (ret != ISC_R_SUCCESS) {
		dst_key_free(&key);
		return (ret);
	}

	*keyp = key;
	return (ISC_R_SUCCESS);
}

isc_result_t 
dst_key_tobuffer(const dst_key_t *key, isc_buffer_t *target) {
	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(VALID_KEY(key));
	REQUIRE(target != NULL);

	if (dst_algorithm_supported(key->key_alg) == ISC_FALSE)
		return (DST_R_UNSUPPORTEDALG);

	if (key->func->todns == NULL)
		return (DST_R_UNSUPPORTEDALG);

	return (key->func->todns(key, target));
}

isc_result_t
dst_key_generate(dns_name_t *name, const unsigned int alg,
		 const unsigned int bits, const unsigned int param,
		 const unsigned int flags, const unsigned int protocol,
		 isc_mem_t *mctx, dst_key_t **keyp)
{
	dst_key_t *key;
	isc_result_t ret;

	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(dns_name_isabsolute(name));
	REQUIRE(mctx != NULL);
	REQUIRE(keyp != NULL && *keyp == NULL);

	if (dst_algorithm_supported(alg) == ISC_FALSE)
		return (DST_R_UNSUPPORTEDALG);

	key = get_key_struct(name, alg, flags, protocol, bits, mctx);
	if (key == NULL)
		return (ISC_R_NOMEMORY);

	if (bits == 0) { /* NULL KEY */
		key->key_flags |= DNS_KEYTYPE_NOKEY;
		*keyp = key;
		return (ISC_R_SUCCESS);
	}

	if (key->func->generate == NULL) {
		dst_key_free(&key);
		return (DST_R_UNSUPPORTEDALG);
	}

	ret = key->func->generate(key, param);
	if (ret != ISC_R_SUCCESS) {
		dst_key_free(&key);
		return (ret);
	}

	*keyp = key;
	return (ISC_R_SUCCESS);
}

isc_boolean_t
dst_key_compare(const dst_key_t *key1, const dst_key_t *key2) {
	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(VALID_KEY(key1));
	REQUIRE(VALID_KEY(key2));

	if (key1 == key2)
		return (ISC_TRUE);
	if (key1 == NULL || key2 == NULL)
		return (ISC_FALSE);
	if (key1->key_alg == key2->key_alg &&
	    key1->key_id == key2->key_id &&
	    key1->func->compare != NULL &&
	    key1->func->compare(key1, key2) == ISC_TRUE)
		return (ISC_TRUE);
	else
		return (ISC_FALSE);
}

isc_boolean_t
dst_key_paramcompare(const dst_key_t *key1, const dst_key_t *key2) {
	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(VALID_KEY(key1));
	REQUIRE(VALID_KEY(key2));

	if (key1 == key2)
		return (ISC_TRUE);
	if (key1 == NULL || key2 == NULL)
		return (ISC_FALSE);
	if (key1->key_alg == key2->key_alg &&
	    key1->func->paramcompare != NULL &&
	    key1->func->paramcompare(key1, key2) == ISC_TRUE)
		return (ISC_TRUE);
	else
		return (ISC_FALSE);
}

void
dst_key_free(dst_key_t **keyp) {
	isc_mem_t *mctx;
	dst_key_t *key;

	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(keyp != NULL && VALID_KEY(*keyp));

	key = *keyp;
	mctx = key->mctx;

	INSIST(key->func->destroy != NULL);

	if (key->opaque != NULL)
		key->func->destroy(key);

	dns_name_free(key->key_name, mctx);
	isc_mem_put(mctx, key->key_name, sizeof(dns_name_t));
	memset(key, 0, sizeof(dst_key_t));
	isc_mem_put(mctx, key, sizeof(dst_key_t));
	*keyp = NULL;
}

dns_name_t *
dst_key_name(const dst_key_t *key) {
	REQUIRE(VALID_KEY(key));
	return (key->key_name);
}

unsigned int
dst_key_size(const dst_key_t *key) {
	REQUIRE(VALID_KEY(key));
	return (key->key_size);
}

unsigned int
dst_key_proto(const dst_key_t *key) {
	REQUIRE(VALID_KEY(key));
	return (key->key_proto);
}

unsigned int
dst_key_alg(const dst_key_t *key) {
	REQUIRE(VALID_KEY(key));
	return (key->key_alg);
}

isc_uint32_t
dst_key_flags(const dst_key_t *key) {
	REQUIRE(VALID_KEY(key));
	return (key->key_flags);
}

isc_uint16_t
dst_key_id(const dst_key_t *key) {
	REQUIRE(VALID_KEY(key));
	return (key->key_id);
}

isc_boolean_t
dst_key_isprivate(const dst_key_t *key) {
	REQUIRE(VALID_KEY(key));
	INSIST(key->func->isprivate != NULL);
	return (key->func->isprivate(key));
}

isc_boolean_t
dst_key_iszonekey(const dst_key_t *key) {
	REQUIRE(VALID_KEY(key));
      
	if ((key->key_flags & DNS_KEYTYPE_NOAUTH) != 0)
		return (ISC_FALSE);
	if ((key->key_flags & DNS_KEYFLAG_OWNERMASK) != DNS_KEYOWNER_ZONE)
		return (ISC_FALSE);
	if (key->key_proto != DNS_KEYPROTO_DNSSEC &&
	    key->key_proto != DNS_KEYPROTO_ANY)
		return (ISC_FALSE);
	return (ISC_TRUE);
}

isc_boolean_t
dst_key_isnullkey(const dst_key_t *key) {
	REQUIRE(VALID_KEY(key));
      
	if ((key->key_flags & DNS_KEYFLAG_TYPEMASK) != DNS_KEYTYPE_NOKEY)
		return (ISC_FALSE);
	if ((key->key_flags & DNS_KEYFLAG_OWNERMASK) != DNS_KEYOWNER_ZONE)
		return (ISC_FALSE);
	if (key->key_proto != DNS_KEYPROTO_DNSSEC &&
	    key->key_proto != DNS_KEYPROTO_ANY)
		return (ISC_FALSE);
	return (ISC_TRUE);
}

isc_result_t
dst_key_buildfilename(const dst_key_t *key, const int type,
		      const char *directory, isc_buffer_t *out) {

	REQUIRE(VALID_KEY(key));
	REQUIRE(type == DST_TYPE_PRIVATE || type == DST_TYPE_PUBLIC ||
		type == 0);

	return (buildfilename(key->key_name, key->key_id, key->key_alg,
			      type, directory, out));
}

isc_result_t
dst_key_sigsize(const dst_key_t *key, unsigned int *n) {
	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(VALID_KEY(key));
	REQUIRE(n != NULL);

	switch (key->key_alg) {
		case DST_ALG_RSA:
			*n = (key->key_size + 7) / 8;
			break;
		case DST_ALG_DSA:
			*n = DNS_SIG_DSASIGSIZE;
			break;
		case DST_ALG_HMACMD5:
			*n = 16;
			break;
		case DST_ALG_DH:
		default:
			return (DST_R_UNSUPPORTEDALG);
	}
	return (ISC_R_SUCCESS);
}

isc_result_t
dst_key_secretsize(const dst_key_t *key, unsigned int *n) {
	REQUIRE(dst_initialized == ISC_TRUE);
	REQUIRE(VALID_KEY(key));
	REQUIRE(n != NULL);

	switch (key->key_alg) {
		case DST_ALG_DH:
			*n = (key->key_size + 7) / 8;
			break;
		case DST_ALG_RSA:
		case DST_ALG_DSA:
		case DST_ALG_HMACMD5:
		default:
			return (DST_R_UNSUPPORTEDALG);
	}
	return (ISC_R_SUCCESS);
}

/***
 *** Static methods
 ***/

/* 
 * Allocates a key structure and fills in some of the fields. 
 */
static dst_key_t *
get_key_struct(dns_name_t *name, const unsigned int alg,
	       const unsigned int flags, const unsigned int protocol,
	       const unsigned int bits, isc_mem_t *mctx)
{
	dst_key_t *key; 
	isc_result_t result;

	REQUIRE(dst_algorithm_supported(alg) != ISC_FALSE);

	key = (dst_key_t *) isc_mem_get(mctx, sizeof(dst_key_t));
	if (key == NULL)
		return (NULL);

	memset(key, 0, sizeof(dst_key_t));
	key->magic = KEY_MAGIC;

	key->key_name = isc_mem_get(mctx, sizeof(dns_name_t));
	if (key->key_name == NULL) {
		isc_mem_put(mctx, key, sizeof(dst_key_t));
		return (NULL);
	}
	dns_name_init(key->key_name, NULL);
	result = dns_name_dup(name, mctx, key->key_name);
	if (result != ISC_R_SUCCESS) {
		isc_mem_put(mctx, key->key_name, sizeof(dns_name_t));
		isc_mem_put(mctx, key, sizeof(dst_key_t));
		return (NULL);
	}
	key->key_alg = alg;
	key->key_flags = flags;
	key->key_proto = protocol;
	key->mctx = mctx;
	key->opaque = NULL;
	key->key_size = bits;
	key->func = dst_t_func[alg];
	return (key);
}

/*
 * Reads a public key from disk
 */
static isc_result_t
read_public_key(const char *filename, isc_mem_t *mctx, dst_key_t **keyp) {
	u_char rdatabuf[DST_KEY_MAXSIZE];
	isc_buffer_t b;
	dns_fixedname_t name;
	isc_lex_t *lex = NULL;
	isc_token_t token;
	isc_result_t ret;
	dns_rdata_t rdata;
	unsigned int opt = ISC_LEXOPT_DNSMULTILINE;
	char *newfilename;

	if (strlen(filename) < 8)
		return (DST_R_INVALIDPUBLICKEY);

	newfilename = isc_mem_get(mctx, strlen(filename) + 5);
	if (newfilename == NULL)
		return (ISC_R_NOMEMORY);
	strcpy(newfilename, filename);

	if (strcmp(filename + strlen(filename) - 8, ".private") == 0)
		sprintf(newfilename + strlen(filename) - 8, ".key");
	else if (strcmp(filename + strlen(filename) - 4, ".key") != 0)
		sprintf(newfilename + strlen(filename), ".key");

	/*
	 * Open the file and read its formatted contents
	 * File format:
	 *    domain.name [ttl] [IN] KEY  <flags> <protocol> <algorithm> <key>
	 */

	/* 1500 should be large enough for any key */
	ret = isc_lex_create(mctx, 1500, &lex);
	if (ret != ISC_R_SUCCESS)
		goto cleanup;

	ret = isc_lex_openfile(lex, newfilename);
	if (ret != ISC_R_SUCCESS) {
		if (ret == ISC_R_FILENOTFOUND)
			ret = ISC_R_NOTFOUND;
		goto cleanup;
	}

#define NEXTTOKEN(lex, opt, token) { \
	ret = isc_lex_gettoken(lex, opt, token); \
	if (ret != ISC_R_SUCCESS) \
		goto cleanup; \
	}

#define BADTOKEN() { \
	ret = ISC_R_UNEXPECTEDTOKEN; \
	goto cleanup; \
	}

	/* Read the domain name */
	NEXTTOKEN(lex, opt, &token);
	if (token.type != isc_tokentype_string)
		BADTOKEN();
	dns_fixedname_init(&name);
	isc_buffer_init(&b, token.value.as_pointer,
			strlen(token.value.as_pointer));
	isc_buffer_add(&b, strlen(token.value.as_pointer));
	ret = dns_name_fromtext(dns_fixedname_name(&name), &b, dns_rootname,
				ISC_FALSE, NULL);
	if (ret != ISC_R_SUCCESS)
		goto cleanup;
	
	/* Read the next word: either TTL, 'IN', or 'KEY' */
	NEXTTOKEN(lex, opt, &token);

	/* If it's a TTL, read the next one */
	if (token.type == isc_tokentype_number)
		NEXTTOKEN(lex, opt, &token);
	
	if (token.type != isc_tokentype_string)
		BADTOKEN();

	if (strcasecmp(token.value.as_pointer, "IN") == 0)
		NEXTTOKEN(lex, opt, &token);
	
	if (token.type != isc_tokentype_string)
		BADTOKEN();

	if (strcasecmp(token.value.as_pointer, "KEY") != 0)
		BADTOKEN();
	
	isc_buffer_init(&b, rdatabuf, sizeof(rdatabuf));
	ret = dns_rdata_fromtext(&rdata, dns_rdataclass_in, dns_rdatatype_key,
				 lex, NULL, ISC_FALSE, &b, NULL);
	if (ret != ISC_R_SUCCESS)
		goto cleanup;

	ret = dst_key_fromdns(dns_fixedname_name(&name), &b, mctx, keyp);
	if (ret != ISC_R_SUCCESS)
		goto cleanup;

 cleanup:
	if (lex != NULL) {
		isc_lex_close(lex);
		isc_lex_destroy(&lex);
	}
	isc_mem_put(mctx, newfilename, strlen(filename) + 5);

	return (ret);
}

/*
 * Writes a public key to disk in DNS format.
 */
static isc_result_t
write_public_key(const dst_key_t *key, const char *directory) {
	FILE *fp;
	isc_buffer_t keyb, textb, fileb;
	isc_region_t r;
	char filename[ISC_DIR_NAMEMAX];
	unsigned char key_array[DST_KEY_MAXSIZE];
	char text_array[DST_KEY_MAXSIZE];
	isc_result_t ret;
	isc_result_t dnsret;
	dns_rdata_t rdata;

	REQUIRE(VALID_KEY(key));

	isc_buffer_init(&keyb, key_array, sizeof(key_array));
	isc_buffer_init(&textb, text_array, sizeof(text_array));

	ret = dst_key_todns(key, &keyb);
	if (ret != ISC_R_SUCCESS)
		return (ret);

	isc_buffer_usedregion(&keyb, &r);
	dns_rdata_fromregion(&rdata, dns_rdataclass_in, dns_rdatatype_key, &r);

	dnsret = dns_rdata_totext(&rdata, (dns_name_t *) NULL, &textb);
	if (dnsret != ISC_R_SUCCESS)
		return (DST_R_INVALIDPUBLICKEY);

	isc_buffer_usedregion(&textb, &r);
	
	/*
	 * Make the filename.
	 */
	isc_buffer_init(&fileb, filename, sizeof(filename));
	ret = dst_key_buildfilename(key, DST_TYPE_PUBLIC, directory, &fileb);
	if (ret != ISC_R_SUCCESS)
		return (ret);

	/*
	 * Create public key file.
	 */
	if ((fp = fopen(filename, "w")) == NULL)
		return (DST_R_WRITEERROR);

	ret = dns_name_print(key->key_name, fp);
	if (ret != ISC_R_SUCCESS)
		return (ret);

	fprintf(fp, " IN KEY ");
	fwrite(r.base, 1, r.length, fp);
	fputc('\n', fp);
	fclose(fp);
	return (ISC_R_SUCCESS);
}

static isc_result_t
buildfilename(dns_name_t *name, const unsigned int id, 
	      const unsigned int alg, const unsigned int type,
	      const char *directory, isc_buffer_t *out)
{
	const char *suffix = "";
	unsigned int len;
	isc_result_t result;

	REQUIRE(out != NULL);
	if ((type & DST_TYPE_PRIVATE) != 0)
		suffix = ".private";
	else if (type == DST_TYPE_PUBLIC)
		suffix = ".key";
	if (directory != NULL) {
		if (isc_buffer_availablelength(out) < strlen(directory))
			return (ISC_R_NOSPACE);
		isc_buffer_putstr(out, directory);
		if (strlen(directory) > 0 &&
		    directory[strlen(directory) - 1] != '/')
			isc_buffer_putstr(out, "/");
	}
	if (isc_buffer_availablelength(out) < 1)
		return (ISC_R_NOSPACE);
	isc_buffer_putstr(out, "K");
	result = dns_name_totext(name, ISC_FALSE, out);
	if (result != ISC_R_SUCCESS)
		return (result);
	len = 1 + 3 + 1 + 5 + strlen(suffix) + 1;
	if (isc_buffer_availablelength(out) < len)
		return (ISC_R_NOSPACE);
	sprintf((char *) isc_buffer_used(out), "+%03d+%05d%s", alg, id, suffix);
	isc_buffer_add(out, len);
	return (ISC_R_SUCCESS);
}

void *
dst__mem_alloc(size_t size) {
	INSIST(dst_memory_pool != NULL);
	return (isc_mem_allocate(dst_memory_pool, size));
}

void
dst__mem_free(void *ptr) {
	INSIST(dst_memory_pool != NULL);
	if (ptr != NULL)
		isc_mem_free(dst_memory_pool, ptr);
}

void *
dst__mem_realloc(void *ptr, size_t size) {
	void *p;

	INSIST(dst_memory_pool != NULL);
	p = NULL;
	if (size > 0) {
		p = dst__mem_alloc(size);
		if (p != NULL && ptr != NULL)
			memcpy(p, ptr, size);
	}
	if (ptr != NULL)
		dst__mem_free(ptr);
	return (p);
}

isc_result_t
dst__entropy_getdata(void *buf, unsigned int len, isc_boolean_t pseudo) {
	unsigned int flags = dst_entropy_flags;
	if (pseudo)
		flags &= ~ISC_ENTROPY_GOODONLY;
	return (isc_entropy_getdata(dst_entropy_pool, buf, len, NULL, flags));
}