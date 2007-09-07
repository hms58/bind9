/* Copyright (C) RSA Data Security, Inc. created 1993, 1996.  This is an
   unpublished work protected as such under copyright law.  This work
   contains proprietary, confidential, and trade secret information of
   RSA Data Security, Inc.  Use, disclosure or reproduction without the
   express written authorization of RSA Data Security, Inc. is
   prohibited.
 */

#ifndef DNSSAFE_KIPKCRPR_H
#define DNSSAFE_KIPKCRPR_H 1

extern B_KeyInfoType KIT_PKCS_RSAPrivate;

int KIT_PKCS_RSAPrivateAddInfo PROTO_LIST ((B_Key *, POINTER));
int KIT_PKCS_RSAPrivateMakeInfo PROTO_LIST ((POINTER *, B_Key *));

#endif /* DNSSAFE_KIPKCRPR_H */