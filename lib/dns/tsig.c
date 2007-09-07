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

/*
 * $Id: tsig.c,v 1.72 2000/06/23 00:48:28 bwelling Exp $
 * Principal Author: Brian Wellington
 */

#include <config.h>
#include <stdlib.h>		/* Required for abs(). */

#include <isc/buffer.h>
#include <isc/mem.h>
#include <isc/once.h>
#include <isc/string.h>		/* Required for HP/UX (and others?) */
#include <isc/util.h>

#include <dns/keyvalues.h>
#include <dns/message.h>
#include <dns/rdata.h>
#include <dns/rdatalist.h>
#include <dns/rdataset.h>
#include <dns/rdatastruct.h>
#include <dns/result.h>
#include <dns/tsig.h>

#include <dst/result.h>

#define TSIG_MAGIC		0x54534947	/* TSIG */
#define VALID_TSIG_KEY(x)	ISC_MAGIC_VALID(x, TSIG_MAGIC)

#define is_response(msg) (msg->flags & DNS_MESSAGEFLAG_QR)

static isc_once_t once = ISC_ONCE_INIT;
static dns_name_t hmacmd5_name;
dns_name_t *dns_tsig_hmacmd5_name = NULL;

static isc_result_t
tsig_verify_tcp(isc_buffer_t *source, dns_message_t *msg);

isc_result_t
dns_tsigkey_create(dns_name_t *name, dns_name_t *algorithm,
		   unsigned char *secret, int length, isc_boolean_t generated,
		   dns_name_t *creator, isc_stdtime_t inception,
		   isc_stdtime_t expire, isc_mem_t *mctx,
		   dns_tsig_keyring_t *ring, dns_tsigkey_t **key)
{
	isc_buffer_t b;
	isc_uint16_t alg;
	dns_tsigkey_t *tkey;
	isc_result_t ret;

	REQUIRE(key == NULL || *key == NULL);
	REQUIRE(name != NULL);
	REQUIRE(algorithm != NULL);
	REQUIRE(length >= 0);
	if (length > 0)
		REQUIRE(secret != NULL);
	REQUIRE(mctx != NULL);

	if (!dns_name_equal(algorithm, DNS_TSIG_HMACMD5_NAME))
		return (ISC_R_NOTFOUND);
	else
		alg = DST_ALG_HMACMD5;

	tkey = (dns_tsigkey_t *) isc_mem_get(mctx, sizeof(dns_tsigkey_t));
	if (tkey == NULL)
		return (ISC_R_NOMEMORY);

	dns_name_init(&tkey->name, NULL);
	ret = dns_name_dup(name, mctx, &tkey->name);
	if (ret != ISC_R_SUCCESS)
		goto cleanup_key;
	dns_name_downcase(&tkey->name, &tkey->name, NULL);

	dns_name_init(&tkey->algorithm, NULL);
	ret = dns_name_dup(algorithm, mctx, &tkey->algorithm);
	if (ret != ISC_R_SUCCESS)
		goto cleanup_name;
	dns_name_downcase(&tkey->algorithm, &tkey->algorithm, NULL);

	if (creator != NULL) {
		tkey->creator = isc_mem_get(mctx, sizeof(dns_name_t));
		if (tkey->creator == NULL) {
			ret = ISC_R_NOMEMORY;
			goto cleanup_algorithm;
		}
		dns_name_init(tkey->creator, NULL);
		ret = dns_name_dup(algorithm, mctx, tkey->creator);
		if (ret != ISC_R_SUCCESS) {
			isc_mem_put(mctx, tkey->creator, sizeof(dns_name_t));
			goto cleanup_algorithm;
		}
	}
	else
		tkey->creator = NULL;

	tkey->key = NULL;
	tkey->ring = NULL;
	if (length > 0) {
		dns_tsigkey_t *tmp;

		isc_buffer_init(&b, secret, length);
		isc_buffer_add(&b, length);
		ret = dst_key_frombuffer(name, alg,
					 DNS_KEYOWNER_ENTITY,
					 DNS_KEYPROTO_DNSSEC,
					 &b, mctx, &tkey->key);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_algorithm;

		ISC_LINK_INIT(tkey, link);
		isc_rwlock_lock(&ring->lock, isc_rwlocktype_write);
		tmp = ISC_LIST_HEAD(ring->keys);
		while (tmp != NULL) {
			if (dns_name_equal(&tkey->name, &tmp->name) &&
			    !tmp->deleted)
			{
				ret = ISC_R_EXISTS;
				isc_rwlock_unlock(&ring->lock,
						  isc_rwlocktype_write);
				goto cleanup_algorithm;
			}
			tmp = ISC_LIST_NEXT(tmp, link);
		}
		ISC_LIST_APPEND(ring->keys, tkey, link);
		isc_rwlock_unlock(&ring->lock, isc_rwlocktype_write);
		tkey->ring = ring;
	}

	tkey->refs = 0;
	if (key != NULL)
		tkey->refs++;
	tkey->generated = generated;
	tkey->inception = inception;
	tkey->expire = expire;
	tkey->deleted = ISC_FALSE;
	tkey->mctx = mctx;
	ret = isc_mutex_init(&tkey->lock);
	if (ret != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_mutex_init() failed: %s",
				 isc_result_totext(ret));
		return (ISC_R_UNEXPECTED);
	}
	
	tkey->magic = TSIG_MAGIC;

	if (key != NULL)
		*key = tkey;

	return (ISC_R_SUCCESS);

cleanup_algorithm:
	dns_name_free(&tkey->algorithm, mctx);
cleanup_name:
	dns_name_free(&tkey->name, mctx);
cleanup_key:
	isc_mem_put(mctx, *key, sizeof(dns_tsigkey_t));

	return (ret);
}

void
dns_tsigkey_attach(dns_tsigkey_t *source, dns_tsigkey_t **targetp) {
	REQUIRE(VALID_TSIG_KEY(source));
	REQUIRE(targetp != NULL && *targetp == NULL);

	isc_mutex_lock(&source->lock);
	source->refs++;
	isc_mutex_unlock(&source->lock);
	*targetp = source;
}

static void
tsigkey_free(dns_tsigkey_t *key) {
	dns_tsig_keyring_t *ring;

	REQUIRE(VALID_TSIG_KEY(key));
	ring = key->ring;

	key->magic = 0;
	if (key->key != NULL) {
		isc_rwlock_lock(&ring->lock, isc_rwlocktype_write);
		ISC_LIST_UNLINK(ring->keys, key, link);
		isc_rwlock_unlock(&ring->lock, isc_rwlocktype_write);
	}
	dns_name_free(&key->name, key->mctx);
	dns_name_free(&key->algorithm, key->mctx);
	if (key->key != NULL)
		dst_key_free(&key->key);
	if (key->creator != NULL) {
		dns_name_free(key->creator, key->mctx);
		isc_mem_put(key->mctx, key->creator, sizeof(dns_name_t));
	}
	isc_mem_put(key->mctx, key, sizeof(dns_tsigkey_t));
}

void
dns_tsigkey_detach(dns_tsigkey_t **key) {
	dns_tsigkey_t *tkey;

	REQUIRE(key != NULL);
	REQUIRE(VALID_TSIG_KEY(*key));
	tkey = *key;
	*key = NULL;

	isc_mutex_lock(&tkey->lock);
	tkey->refs--;
	if (tkey->refs > 0 || (!tkey->deleted && tkey->key != NULL)) {
		isc_mutex_unlock(&tkey->lock);
		return;
	}
	isc_mutex_unlock(&tkey->lock);
	tsigkey_free(tkey);
}

void
dns_tsigkey_setdeleted(dns_tsigkey_t *key) {
	INSIST(VALID_TSIG_KEY(key));
	isc_mutex_lock(&key->lock);
	key->deleted = ISC_TRUE;
	isc_mutex_unlock(&key->lock);
}

isc_result_t
dns_tsig_sign(dns_message_t *msg) {
	dns_tsigkey_t *key;
	dns_rdata_any_tsig_t tsig, querytsig;
	unsigned char data[128];
	isc_buffer_t databuf, sigbuf;
	isc_buffer_t *dynbuf;
	dns_name_t *owner;
	dns_rdata_t *rdata;
	dns_rdatalist_t *datalist;
	dns_rdataset_t *dataset;
	isc_region_t r, r2;
	isc_stdtime_t now;
	isc_mem_t *mctx;
	dst_context_t *ctx = NULL;
	isc_result_t ret;

	REQUIRE(msg != NULL);
	REQUIRE(VALID_TSIG_KEY(dns_message_gettsigkey(msg)));

	/*
	 * If this is a response, there should be a query tsig.
	 */
	if (is_response(msg) && msg->querytsig == NULL)
		return (DNS_R_EXPECTEDTSIG);

	dynbuf = NULL;

	mctx = msg->mctx;
	key = dns_message_gettsigkey(msg);

	tsig.mctx = mctx;
	tsig.common.rdclass = dns_rdataclass_any;
	tsig.common.rdtype = dns_rdatatype_tsig;
	ISC_LINK_INIT(&tsig.common, link);
	dns_name_init(&tsig.algorithm, NULL);
	dns_name_clone(&key->algorithm, &tsig.algorithm);

	isc_stdtime_get(&now);
	tsig.timesigned = now;
	tsig.fudge = DNS_TSIG_FUDGE;

	tsig.originalid = msg->id;

	isc_buffer_init(&databuf, data, sizeof(data));

	if (is_response(msg))
		tsig.error = msg->querytsigstatus;
	else
		tsig.error = dns_rcode_noerror;

	if (tsig.error != dns_tsigerror_badtime) {
		tsig.otherlen = 0;
		tsig.other = NULL;
	}
	else {
		isc_buffer_t otherbuf;
		tsig.otherlen = 6;
		tsig.other = (unsigned char *)isc_mem_get(mctx, tsig.otherlen);
		if (tsig.other == NULL) {
			ret = ISC_R_NOMEMORY;
			goto cleanup_other;
		}
		isc_buffer_init(&otherbuf, tsig.other, tsig.otherlen);
		isc_buffer_putuint16(&otherbuf,
				     (isc_uint16_t)(tsig.timesigned >> 32));
		isc_buffer_putuint32(&otherbuf,
				     (isc_uint32_t)(tsig.timesigned &
						    0xFFFFFFFF));
		
	}
	if (!dns_tsigkey_empty(key) && tsig.error != dns_tsigerror_badsig) {
		unsigned char header[DNS_MESSAGE_HEADERLEN];
		isc_buffer_t headerbuf;
		unsigned int sigsize;

		ret = dst_context_create(key->key, mctx, &ctx);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_other;

		/*
		 * If this is a response, digest the query signature.
		 */
		if (is_response(msg)) {
			dns_rdata_t querytsigrdata;

			ret = dns_rdataset_first(msg->querytsig);
			if (ret != ISC_R_SUCCESS)
				goto cleanup_context;
			dns_rdataset_current(msg->querytsig, &querytsigrdata);
			ret = dns_rdata_tostruct(&querytsigrdata, &querytsig,
						 NULL);
			if (ret != ISC_R_SUCCESS)
				goto cleanup_context;
			isc_buffer_putuint16(&databuf, querytsig.siglen);
			if (isc_buffer_availablelength(&databuf) <
			    querytsig.siglen)
			{
				ret = ISC_R_NOSPACE;
				goto cleanup_context;
			}
			isc_buffer_putmem(&databuf, querytsig.signature,
					  querytsig.siglen);
			isc_buffer_usedregion(&databuf, &r);
			ret = dst_context_adddata(ctx, &r);
			if (ret != ISC_R_SUCCESS)
				goto cleanup_context;
		}

		/*
		 * Digest the header.
		 */
		isc_buffer_init(&headerbuf, header, sizeof(header));
		dns_message_renderheader(msg, &headerbuf);
		isc_buffer_usedregion(&headerbuf, &r);
		ret = dst_context_adddata(ctx, &r);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_context;

		/*
		 * Digest the remainder of the message.
		 */
		isc_buffer_usedregion(msg->buffer, &r);
		isc_region_consume(&r, DNS_MESSAGE_HEADERLEN);
		ret = dst_context_adddata(ctx, &r);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_context;

		if (msg->tcp_continuation == 0) {
			/*
			 * Digest the name, class, ttl, alg.
			 */
			dns_name_toregion(&key->name, &r);
			ret = dst_context_adddata(ctx, &r);
			if (ret != ISC_R_SUCCESS)
				goto cleanup_context;

			isc_buffer_clear(&databuf);
			isc_buffer_putuint16(&databuf, dns_rdataclass_any);
			isc_buffer_putuint32(&databuf, 0); /* ttl */
			isc_buffer_usedregion(&databuf, &r);
			ret = dst_context_adddata(ctx, &r);
			if (ret != ISC_R_SUCCESS)
				goto cleanup_context;

			dns_name_toregion(&tsig.algorithm, &r);
			ret = dst_context_adddata(ctx, &r);
			if (ret != ISC_R_SUCCESS)
				goto cleanup_context;

		}
		/* Digest the timesigned and fudge */
		isc_buffer_clear(&databuf);
		if (tsig.error != dns_tsigerror_badtime) {
			isc_buffer_putuint16(&databuf,
					     (isc_uint16_t)(tsig.timesigned >>
							    32));
			isc_buffer_putuint32(&databuf,
					     (isc_uint32_t)(tsig.timesigned &
							    0xFFFFFFFF));
		}
		else {
   			isc_uint64_t querysigned = querytsig.timesigned;
			isc_buffer_putuint16(&databuf,
					     (isc_uint16_t)(querysigned >>
							    32));
			isc_buffer_putuint32(&databuf,
					     (isc_uint16_t)(querysigned &
							    0xFFFFFFFF));
		}
		isc_buffer_putuint16(&databuf, tsig.fudge);
		isc_buffer_usedregion(&databuf, &r);
		ret = dst_context_adddata(ctx, &r);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_context;

		if (msg->tcp_continuation == 0) {
			/*
			 * Digest the error and other data length.
			 */
			isc_buffer_clear(&databuf);
			isc_buffer_putuint16(&databuf, tsig.error);
			isc_buffer_putuint16(&databuf, tsig.otherlen);

			isc_buffer_usedregion(&databuf, &r);
			ret = dst_context_adddata(ctx, &r);
			if (ret != ISC_R_SUCCESS)
				goto cleanup_context;

			/*
			 * Digest the error and other data.
			 */
			if (tsig.otherlen > 0) {
				r.length = tsig.otherlen;
				r.base = tsig.other;
				ret = dst_context_adddata(ctx, &r);
				if (ret != ISC_R_SUCCESS)
					goto cleanup_context;
			}
		}

		ret = dst_key_sigsize(key->key, &sigsize);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_context;
		tsig.siglen = sigsize;
		tsig.signature = (unsigned char *)
				  isc_mem_get(mctx, tsig.siglen);
		if (tsig.signature == NULL) {
			ret = ISC_R_NOMEMORY;
			goto cleanup_context;
		}

		isc_buffer_init(&sigbuf, tsig.signature, tsig.siglen);
		ret = dst_context_sign(ctx, &sigbuf);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_signature;
		dst_context_destroy(&ctx);
	}
	else {
		tsig.siglen = 0;
		tsig.signature = NULL;
	}

	rdata = NULL;
	ret = dns_message_gettemprdata(msg, &rdata);
	if (ret != ISC_R_SUCCESS)
		goto cleanup_signature;
	ret = isc_buffer_allocate(msg->mctx, &dynbuf, 512);
	if (ret != ISC_R_SUCCESS)
		goto cleanup_signature;
	ret = dns_rdata_fromstruct(rdata, dns_rdataclass_any,
				   dns_rdatatype_tsig, &tsig, dynbuf);
	if (ret != ISC_R_SUCCESS)
		goto cleanup_dynbuf;

	dns_message_takebuffer(msg, &dynbuf);

	if (tsig.signature != NULL) {
		isc_mem_put(mctx, tsig.signature, tsig.siglen);
		tsig.signature = NULL;
	}
	if (tsig.other != NULL) {
		isc_mem_put(mctx, tsig.other, tsig.otherlen);
		tsig.other = NULL;
	}

	owner = NULL;
	ret = dns_message_gettempname(msg, &owner);
	if (ret != ISC_R_SUCCESS)
		goto cleanup_dynbuf;
	dns_name_toregion(&key->name, &r);
	dynbuf = NULL;
	ret = isc_buffer_allocate(mctx, &dynbuf, r.length);
	if (ret != ISC_R_SUCCESS)
		goto cleanup_dynbuf;
	isc_buffer_availableregion(dynbuf, &r2);
	memcpy(r2.base, r.base, r.length);
	dns_name_init(owner, NULL);
	dns_name_fromregion(owner, &r2);
	dns_message_takebuffer(msg, &dynbuf);

	datalist = NULL;
	ret = dns_message_gettemprdatalist(msg, &datalist);
	if (ret != ISC_R_SUCCESS)
		goto cleanup_dynbuf;
	datalist->rdclass = dns_rdataclass_any;
	datalist->type = dns_rdatatype_tsig;
	datalist->covers = 0;
	datalist->ttl = 0;
	ISC_LIST_INIT(datalist->rdata);
	ISC_LIST_APPEND(datalist->rdata, rdata, link);
	dataset = NULL;
	ret = dns_message_gettemprdataset(msg, &dataset);
	if (ret != ISC_R_SUCCESS)
		goto cleanup_dynbuf;
	dns_rdataset_init(dataset);
	dns_rdatalist_tordataset(datalist, dataset);
	msg->tsig = dataset;
	msg->tsigname = owner;

	return (ISC_R_SUCCESS);

cleanup_dynbuf:
	if (dynbuf != NULL)
		isc_buffer_free(&dynbuf);
cleanup_signature:
	if (tsig.signature != NULL)
		isc_mem_put(mctx, tsig.signature, tsig.siglen);
cleanup_context:
	if (ctx != NULL)
		dst_context_destroy(&ctx);
cleanup_other:
	if (tsig.other != NULL)
		isc_mem_put(mctx, tsig.other, tsig.otherlen);

	return (ret);
}

isc_result_t
dns_tsig_verify(isc_buffer_t *source, dns_message_t *msg,
		dns_tsig_keyring_t *sring, dns_tsig_keyring_t *dring)
{
	dns_rdata_any_tsig_t tsig, querytsig;
	isc_region_t r, source_r, header_r, sig_r;
	isc_buffer_t databuf;
	unsigned char data[32];
	dns_name_t *keyname;
	dns_rdata_t rdata;
	isc_stdtime_t now;
	isc_result_t ret;
	dns_tsigkey_t *tsigkey;
	dst_key_t *key = NULL;
	unsigned char header[DNS_MESSAGE_HEADERLEN];
	dst_context_t *ctx = NULL;
	isc_mem_t *mctx;
	isc_uint16_t addcount, id;

	REQUIRE(source != NULL);
	REQUIRE(DNS_MESSAGE_VALID(msg));
	tsigkey = dns_message_gettsigkey(msg);
	REQUIRE(tsigkey == NULL || VALID_TSIG_KEY(tsigkey));

	msg->verify_attempted = 1;

	if (msg->tcp_continuation)
		return (tsig_verify_tcp(source, msg));

	/*
	 * There should be a TSIG record...
	 */
	if (msg->tsig == NULL)
		return (DNS_R_EXPECTEDTSIG);

	/*
	 * If this is a response and there's no key or query TSIG, there
	 * shouldn't be one on the response.
	 */
	if (is_response(msg) &&
	    (tsigkey == NULL || msg->querytsig == NULL))
		return (DNS_R_UNEXPECTEDTSIG);

	mctx = msg->mctx;

	/*
	 * If we're here, we know the message is well formed and contains a
	 * TSIG record.
	 */

	keyname = msg->tsigname;
	ret = dns_rdataset_first(msg->tsig);
	if (ret != ISC_R_SUCCESS)
		return (ret);
	dns_rdataset_current(msg->tsig, &rdata);
	ret = dns_rdata_tostruct(&rdata, &tsig, NULL);
	if (ret != ISC_R_SUCCESS)
		return (ret);
	if (is_response(msg)) {
		ret = dns_rdataset_first(msg->querytsig);
		if (ret != ISC_R_SUCCESS)
			return (ret);
		dns_rdataset_current(msg->querytsig, &rdata);
		ret = dns_rdata_tostruct(&rdata, &querytsig, NULL);
		if (ret != ISC_R_SUCCESS)
			return (ret);
	}
	
	/*
	 * Do the key name and algorithm match that of the query?
	 */
	if (is_response(msg) &&
	    (!dns_name_equal(keyname, &tsigkey->name) ||
	     !dns_name_equal(&tsig.algorithm, &querytsig.algorithm)))
	{
		msg->tsigstatus = dns_tsigerror_badkey;
		return (DNS_R_TSIGVERIFYFAILURE);
	}

	/*
	 * Get the current time.
	 */
	isc_stdtime_get(&now);

	/*
	 * Find dns_tsigkey_t based on keyname.
	 */
	if (tsigkey == NULL) {
		ret = ISC_R_NOTFOUND;
		if (sring != NULL)
			ret = dns_tsigkey_find(&tsigkey, keyname,
					       &tsig.algorithm, sring); 
		if (ret == ISC_R_NOTFOUND && dring != NULL)
			ret = dns_tsigkey_find(&tsigkey, keyname,
					       &tsig.algorithm, dring);
		if (ret != ISC_R_SUCCESS) {
			if (dring == NULL)
				return (DNS_R_TSIGVERIFYFAILURE);
			msg->tsigstatus = dns_tsigerror_badkey;
			ret = dns_tsigkey_create(keyname, &tsig.algorithm,
						 NULL, 0, ISC_FALSE, NULL,
						 now, now,
						 mctx, dring, &msg->tsigkey);
			if (ret != ISC_R_SUCCESS)
				return (ret);
			return (DNS_R_TSIGVERIFYFAILURE);
		}
		msg->tsigkey = tsigkey;
	}

	key = tsigkey->key;

	/*
	 * Is the time ok?
	 */
	if (abs(now - tsig.timesigned) > tsig.fudge) {
		msg->tsigstatus = dns_tsigerror_badtime;
		return (DNS_R_TSIGVERIFYFAILURE);
	}

	if (tsig.siglen > 0) {
		sig_r.base = tsig.signature;
		sig_r.length = tsig.siglen;

		ret = dst_context_create(key, mctx, &ctx);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_key;

		if (is_response(msg)) {
			isc_buffer_init(&databuf, data, sizeof(data));
			isc_buffer_putuint16(&databuf, querytsig.siglen);
			isc_buffer_usedregion(&databuf, &r);
			ret = dst_context_adddata(ctx, &r);
			if (ret != ISC_R_SUCCESS)
				goto cleanup_context;
			if (querytsig.siglen > 0) {
				r.length = querytsig.siglen;
				r.base = querytsig.signature;
				ret = dst_context_adddata(ctx, &r);
				if (ret != ISC_R_SUCCESS)
					goto cleanup_context;
			}
		}

		/*
		 * Extract the header.
		 */
		isc_buffer_usedregion(source, &r);
		memcpy(header, r.base, DNS_MESSAGE_HEADERLEN);
		isc_region_consume(&r, DNS_MESSAGE_HEADERLEN);

		/*
		 * Decrement the additional field counter.
		 */
		memcpy(&addcount, &header[DNS_MESSAGE_HEADERLEN - 2], 2);
		addcount = htons(ntohs(addcount) - 1);
		memcpy(&header[DNS_MESSAGE_HEADERLEN - 2], &addcount, 2);

		/*
		 * Put in the original id.
		 */
		id = htons(tsig.originalid);
		memcpy(&header[0], &id, 2);

		/*
		 * Digest the modified header.
		 */
		header_r.base = (unsigned char *) header;
		header_r.length = DNS_MESSAGE_HEADERLEN;
		ret = dst_context_adddata(ctx, &header_r);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_context;

		/*
		 * Digest all non-TSIG records.
		 */
		isc_buffer_usedregion(source, &source_r);
		r.base = source_r.base + DNS_MESSAGE_HEADERLEN;
		r.length = msg->sigstart - DNS_MESSAGE_HEADERLEN;
		ret = dst_context_adddata(ctx, &r);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_context;

		/*
		 * Digest the key name.
		 */
		dns_name_toregion(&tsigkey->name, &r);
		ret = dst_context_adddata(ctx, &r);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_context;

		isc_buffer_init(&databuf, data, sizeof(data));
		isc_buffer_putuint16(&databuf, tsig.common.rdclass);
		isc_buffer_putuint32(&databuf, msg->tsig->ttl);
		isc_buffer_usedregion(&databuf, &r);
		ret = dst_context_adddata(ctx, &r);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_context;

		/*
		 * Digest the key algorithm.
		 */
		dns_name_toregion(&tsigkey->algorithm, &r);
		ret = dst_context_adddata(ctx, &r);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_context;

		isc_buffer_clear(&databuf);
		isc_buffer_putuint16(&databuf, (isc_uint16_t)(tsig.timesigned
							      >> 32));
		isc_buffer_putuint32(&databuf, (isc_uint32_t)(tsig.timesigned
							      & 0xFFFFFFFF));
		isc_buffer_putuint16(&databuf, tsig.fudge);
		isc_buffer_putuint16(&databuf, tsig.error);
		isc_buffer_putuint16(&databuf, tsig.otherlen);
		isc_buffer_usedregion(&databuf, &r);
		ret = dst_context_adddata(ctx, &r);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_context;

		if (tsig.otherlen > 0) {
			r.base = tsig.other;
			r.length = tsig.otherlen;
			ret = dst_context_adddata(ctx, &r);
			if (ret != ISC_R_SUCCESS)
				goto cleanup_context;
		}

		ret = dst_context_verify(ctx, &sig_r);
		if (ret == DST_R_VERIFYFAILURE) {
			msg->tsigstatus = dns_tsigerror_badsig;
			ret = DNS_R_TSIGVERIFYFAILURE;
			goto cleanup_context;
		}
		else if (ret != ISC_R_SUCCESS)
			goto cleanup_context;

		dst_context_destroy(&ctx);
	}
	else if (tsig.error != dns_tsigerror_badsig &&
		 tsig.error != dns_tsigerror_badkey)
	{
		msg->tsigstatus = dns_tsigerror_badsig;
		return (DNS_R_TSIGVERIFYFAILURE);
	}

	msg->tsigstatus = dns_rcode_noerror;

	if (tsig.error != dns_rcode_noerror) {
		if (is_response(msg)) {
			/* XXXBEW Log a message */
			return (ISC_R_SUCCESS);
		}
		else
			return (DNS_R_TSIGERRORSET);
	}

	msg->verified_sig = 1;

	return (ISC_R_SUCCESS);

cleanup_context:
	if (ctx != NULL)
		dst_context_destroy(&ctx);
cleanup_key:
	if (dns_tsigkey_empty(tsigkey))
		dns_tsigkey_detach(&tsigkey);

	return (ret);
}

static isc_result_t
tsig_verify_tcp(isc_buffer_t *source, dns_message_t *msg) {
	dns_rdata_any_tsig_t tsig, querytsig;
	isc_region_t r, source_r, header_r, sig_r;
	isc_buffer_t databuf;
	unsigned char data[32];
	dns_name_t *keyname;
	dns_rdata_t rdata;
	isc_stdtime_t now;
	isc_result_t ret;
	dns_tsigkey_t *tsigkey;
	dst_key_t *key = NULL;
	unsigned char header[DNS_MESSAGE_HEADERLEN];
	isc_uint16_t addcount, id;
	isc_boolean_t has_tsig = ISC_FALSE;
	isc_mem_t *mctx;

	REQUIRE(source != NULL);
	REQUIRE(msg != NULL);
	REQUIRE(dns_message_gettsigkey(msg) != NULL);
	REQUIRE(msg->tcp_continuation == 1);
	REQUIRE(is_response(msg));
	REQUIRE(msg->querytsig != NULL);

	mctx = msg->mctx;

	tsigkey = dns_message_gettsigkey(msg);

	/*
	 * Extract and parse the previous TSIG
	 */
	ret = dns_rdataset_first(msg->querytsig);
	if (ret != ISC_R_SUCCESS)
		return (ret);
	dns_rdataset_current(msg->querytsig, &rdata);
	ret = dns_rdata_tostruct(&rdata, &querytsig, NULL);
	if (ret != ISC_R_SUCCESS)
		return (ret);

	/*
	 * If there is a TSIG in this message, do some checks.
	 */
	if (msg->tsig != NULL) {
		has_tsig = ISC_TRUE;

		keyname = msg->tsigname;
		ret = dns_rdataset_first(msg->tsig);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_querystruct;
		dns_rdataset_current(msg->tsig, &rdata);
		ret = dns_rdata_tostruct(&rdata, &tsig, NULL);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_querystruct;
	
		/*
		 * Do the key name and algorithm match that of the query?
		 */
		if (!dns_name_equal(keyname, &tsigkey->name) ||
		    !dns_name_equal(&tsig.algorithm, &querytsig.algorithm))
		{
			msg->tsigstatus = dns_tsigerror_badkey;
			ret = DNS_R_TSIGVERIFYFAILURE;
			goto cleanup_querystruct;
		}

		/*
		 * Is the time ok?
		 */
		isc_stdtime_get(&now);
		if (abs(now - tsig.timesigned) > tsig.fudge) {
			msg->tsigstatus = dns_tsigerror_badtime;
			ret = DNS_R_TSIGVERIFYFAILURE;
			goto cleanup_querystruct;
		}
	}

	key = tsigkey->key;

	if (msg->tsigctx == NULL) {
		ret = dst_context_create(key, mctx, &msg->tsigctx);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_querystruct;

		/*
		 * Digest the length of the query signature
		 */
		isc_buffer_init(&databuf, data, sizeof(data));
		isc_buffer_putuint16(&databuf, querytsig.siglen);
		isc_buffer_usedregion(&databuf, &r);
		ret = dst_context_adddata(msg->tsigctx, &r);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_context;

		/*
		 * Digest the data of the query signature
		 */
		if (querytsig.siglen > 0) {
			r.length = querytsig.siglen;
			r.base = querytsig.signature;
			ret = dst_context_adddata(msg->tsigctx, &r);
			if (ret != ISC_R_SUCCESS)
				goto cleanup_context;
		}
	}

	/*
	 * Extract the header.
	 */
	isc_buffer_usedregion(source, &r);
	memcpy(header, r.base, DNS_MESSAGE_HEADERLEN);
	isc_region_consume(&r, DNS_MESSAGE_HEADERLEN);

	/*
	 * Decrement the additional field counter if necessary.
	 */
	if (has_tsig) {
		memcpy(&addcount, &header[DNS_MESSAGE_HEADERLEN - 2], 2);
		addcount = htons(ntohs(addcount) - 1);
		memcpy(&header[DNS_MESSAGE_HEADERLEN - 2], &addcount, 2);
	}

	/*
	 * Put in the original id.
	 */
	/* XXX Can TCP transfers be forwarded?  How would that work? */
	if (has_tsig) {
		id = htons(tsig.originalid);
		memcpy(&header[0], &id, 2);
	}

	/*
	 * Digest the modified header.
	 */
	header_r.base = (unsigned char *) header;
	header_r.length = DNS_MESSAGE_HEADERLEN;
	ret = dst_context_adddata(msg->tsigctx, &header_r);
	if (ret != ISC_R_SUCCESS)
		goto cleanup_context;

	/*
	 * Digest all non-TSIG records.
	 */
	isc_buffer_usedregion(source, &source_r);
	r.base = source_r.base + DNS_MESSAGE_HEADERLEN;
	if (has_tsig)
		r.length = msg->sigstart - DNS_MESSAGE_HEADERLEN;
	else
		r.length = source_r.length - DNS_MESSAGE_HEADERLEN;
	ret = dst_context_adddata(msg->tsigctx, &r);
	if (ret != ISC_R_SUCCESS)
		goto cleanup_context;

	/*
	 * Digest the time signed and fudge.
	 */
	if (has_tsig) {
		isc_buffer_init(&databuf, data, sizeof(data));
		isc_buffer_putuint16(&databuf, (isc_uint16_t)(tsig.timesigned
							      >> 32));
		isc_buffer_putuint32(&databuf, (isc_uint32_t)(tsig.timesigned
							      & 0xFFFFFFFF));
		isc_buffer_putuint16(&databuf, tsig.fudge);
		isc_buffer_usedregion(&databuf, &r);
		ret = dst_context_adddata(msg->tsigctx, &r);
		if (ret != ISC_R_SUCCESS)
			goto cleanup_context;

		sig_r.base = tsig.signature;
		sig_r.length = tsig.siglen;
		if (tsig.siglen == 0) {
			if (tsig.error != dns_rcode_noerror)
				ret = DNS_R_TSIGERRORSET;
			else
				ret = DNS_R_TSIGVERIFYFAILURE;
			goto cleanup_context;
		}

		ret = dst_context_verify(msg->tsigctx, &sig_r);
		if (ret == DST_R_VERIFYFAILURE) {
			msg->tsigstatus = dns_tsigerror_badsig;
			ret = DNS_R_TSIGVERIFYFAILURE;
			goto cleanup_context;
		}
		else if (ret != ISC_R_SUCCESS)
			goto cleanup_context;

		dst_context_destroy(&msg->tsigctx);
	}

	msg->tsigstatus = dns_rcode_noerror;
	return (ISC_R_SUCCESS);

 cleanup_context:
	dst_context_destroy(&msg->tsigctx);

 cleanup_querystruct:
	dns_rdata_freestruct(&querytsig);

	return (ret);

}

isc_result_t
dns_tsigkey_find(dns_tsigkey_t **tsigkey, dns_name_t *name,
		 dns_name_t *algorithm, dns_tsig_keyring_t *ring)
{
	dns_tsigkey_t *key;
	isc_stdtime_t now;

	REQUIRE(tsigkey != NULL);
	REQUIRE(*tsigkey == NULL);
	REQUIRE(name != NULL);
	REQUIRE(ring != NULL);

	isc_stdtime_get(&now);
	isc_rwlock_lock(&ring->lock, isc_rwlocktype_read);
	key = ISC_LIST_HEAD(ring->keys);
	while (key != NULL) {
		if (dns_name_equal(&key->name, name) &&
		    (algorithm == NULL ||
		     dns_name_equal(&key->algorithm, algorithm)) &&
		    !key->deleted)
		{
			if (key->inception != key->expire &&
			    key->expire < now)
			{
				/*
				 * The key has expired.
				 */
				key->deleted = ISC_TRUE;
				continue;
			}
			isc_mutex_lock(&key->lock);
			key->refs++;
			isc_mutex_unlock(&key->lock);
			*tsigkey = key;
			isc_rwlock_unlock(&ring->lock, isc_rwlocktype_read);
			return (ISC_R_SUCCESS);
		}
		key = ISC_LIST_NEXT(key, link);
	}
	isc_rwlock_unlock(&ring->lock, isc_rwlocktype_read);
	*tsigkey = NULL;
	return (ISC_R_NOTFOUND);
}

static void
dns_tsig_inithmac(void) {
	isc_constregion_t r;
	const char *str = "\010HMAC-MD5\007SIG-ALG\003REG\003INT";

	dns_name_init(&hmacmd5_name, NULL);
	r.base = str;
	r.length = strlen(str) + 1;
	dns_name_fromregion(&hmacmd5_name, (isc_region_t *)&r);
	dns_tsig_hmacmd5_name = &hmacmd5_name;
}

isc_result_t
dns_tsigkeyring_create(isc_mem_t *mctx, dns_tsig_keyring_t **ring) {
	isc_result_t ret;
	
	REQUIRE(mctx != NULL);
	REQUIRE(ring != NULL);
	REQUIRE(*ring == NULL);

	RUNTIME_CHECK(isc_once_do(&once, dns_tsig_inithmac) == ISC_R_SUCCESS);
	*ring = isc_mem_get(mctx, sizeof(dns_tsig_keyring_t));
	if (ring == NULL)
		return (ISC_R_NOMEMORY);
		
	ret = isc_rwlock_init(&(*ring)->lock, 0, 0);
	if (ret != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_rwlock_init() failed: %s",
				 isc_result_totext(ret));
		return (ISC_R_UNEXPECTED);
	}
	
	ISC_LIST_INIT((*ring)->keys);

	(*ring)->mctx = mctx;

	return (ISC_R_SUCCESS);
}

void
dns_tsigkeyring_destroy(dns_tsig_keyring_t **ring) {
	isc_mem_t *mctx;

	REQUIRE(ring != NULL);
	REQUIRE(*ring != NULL);

	while (!ISC_LIST_EMPTY((*ring)->keys)) {
		dns_tsigkey_t *key = ISC_LIST_HEAD((*ring)->keys);
		key->refs = 0;
		key->deleted = ISC_TRUE;
		tsigkey_free(key);
	}
	isc_rwlock_destroy(&(*ring)->lock);
	mctx = (*ring)->mctx;
	isc_mem_put(mctx, *ring, sizeof(dns_tsig_keyring_t));

	*ring = NULL;
}