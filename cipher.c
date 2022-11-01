/* $OpenBSD: cipher.c,v 1.119 2021/04/03 06:18:40 djm Exp $ */
/*
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 *
 * As far as I am concerned, the code I have written for this software
 * can be used freely for any purpose.  Any derived versions of this
 * software must be clearly marked as such, and if the derived work is
 * incompatible with the protocol description in the RFC file, it must be
 * called by a name other than "ssh" or "Secure Shell".
 *
 *
 * Copyright (c) 1999 Niels Provos.  All rights reserved.
 * Copyright (c) 1999, 2000 Markus Friedl.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "includes.h"

#include <sys/types.h>

#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "cipher.h"
#include "misc.h"
#include "sshbuf.h"
#include "ssherr.h"
#include "digest.h"
#include "log.h"

#include "openbsd-compat/openssl-compat.h"

/* for provider functions */
#if OPENSSL_VERSION_NUMBER >= 0x30000000UL
#include <openssl/err.h>
#include <openssl/params.h>
#include <openssl/provider.h>
#endif

#ifndef WITH_OPENSSL
#define EVP_CIPHER_CTX void
#else 
/* for multi-threaded aes-ctr cipher */
extern const EVP_CIPHER *evp_aes_ctr_mt(void);
#endif

struct sshcipher_ctx {
	int	plaintext;
	int	encrypt;
	EVP_CIPHER_CTX *evp;
	struct chachapoly_ctx *cp_ctx;
	struct aesctr_ctx ac_ctx; /* XXX union with evp? */
	const struct sshcipher *cipher;
};

struct sshcipher {
	char	*name;
	u_int	block_size;
	u_int	key_len;
	u_int	iv_len;		/* defaults to block_size */
	u_int	auth_len;
	u_int	flags;
#define CFLAG_CBC		(1<<0)
#define CFLAG_CHACHAPOLY	(1<<1)
#define CFLAG_AESCTR		(1<<2)
#define CFLAG_NONE		(1<<3)
#define CFLAG_INTERNAL		CFLAG_NONE /* Don't use "none" for packets */
#ifdef WITH_OPENSSL
	const EVP_CIPHER	*(*evptype)(void);
#else
	void	*ignored;
#endif
};

static struct sshcipher ciphers[] = {
#ifdef WITH_OPENSSL
#ifndef OPENSSL_NO_DES
	{ "3des-cbc",		8, 24, 0, 0, CFLAG_CBC, EVP_des_ede3_cbc },
#endif
	{ "aes128-cbc",		16, 16, 0, 0, CFLAG_CBC, EVP_aes_128_cbc },
	{ "aes192-cbc",		16, 24, 0, 0, CFLAG_CBC, EVP_aes_192_cbc },
	{ "aes256-cbc",		16, 32, 0, 0, CFLAG_CBC, EVP_aes_256_cbc },
	{ "aes128-ctr",		16, 16, 0, 0, 0, EVP_aes_128_ctr },
	{ "aes192-ctr",		16, 24, 0, 0, 0, EVP_aes_192_ctr },
	{ "aes256-ctr",		16, 32, 0, 0, 0, EVP_aes_256_ctr },
	{ "aes128-gcm@openssh.com",
				16, 16, 12, 16, 0, EVP_aes_128_gcm },
	{ "aes256-gcm@openssh.com",
				16, 32, 12, 16, 0, EVP_aes_256_gcm },
#else
	{ "aes128-ctr",		16, 16, 0, 0, CFLAG_AESCTR, NULL },
	{ "aes192-ctr",		16, 24, 0, 0, CFLAG_AESCTR, NULL },
	{ "aes256-ctr",		16, 32, 0, 0, CFLAG_AESCTR, NULL },
#endif
	{ "chacha20-poly1305@openssh.com",
				8, 64, 0, 16, CFLAG_CHACHAPOLY, NULL },
	{ "none",               8, 0, 0, 0, CFLAG_NONE, NULL },

	{ NULL,                 0, 0, 0, 0, 0, NULL }
};

/*--*/

/* Returns a comma-separated list of supported ciphers. */
char *
cipher_alg_list(char sep, int auth_only)
{
	char *tmp, *ret = NULL;
	size_t nlen, rlen = 0;
	const struct sshcipher *c;

	for (c = ciphers; c->name != NULL; c++) {
		if ((c->flags & CFLAG_INTERNAL) != 0)
			continue;
		if (auth_only && c->auth_len == 0)
			continue;
		if (ret != NULL)
			ret[rlen++] = sep;
		nlen = strlen(c->name);
		if ((tmp = realloc(ret, rlen + nlen + 2)) == NULL) {
			free(ret);
			return NULL;
		}
		ret = tmp;
		memcpy(ret + rlen, c->name, nlen + 1);
		rlen += nlen;
	}
	return ret;
}

const char *
compression_alg_list(int compression)
{
#ifdef WITH_ZLIB
	return compression ? "zlib@openssh.com,zlib,none" :
	    "none,zlib@openssh.com,zlib";
#else
	return "none";
#endif
}

/* used to get the cipher name so when force rekeying to handle the
 * single to multithreaded ctr cipher swap we only rekey when appropriate
 */
const char *
cipher_ctx_name(const struct sshcipher_ctx *cc)
{
	return cc->cipher->name;
}

/* in order to get around sandbox and forking issues with a threaded cipher
 * we set the initial pre-auth aes-ctr cipher to the default OpenSSH cipher
 * post auth we set them to the new evp as defined by cipher-ctr-mt
 */
/* we don't need this anymore for the cipher swap. We can just explicitly 
 * give the EVPInit this type -cjr 09/08/2022 
 * function commented out but not deleted to make sure we have a record of it */
/* #ifdef WITH_OPENSSL */
/* void */
/* cipher_reset_multithreaded(void) */
/* { */
/* 	/\* OpenSSL 3.0 introduced a new way of interacting with the EVP */
/* 	 * that makes our method of doing a cipher switch simply not work. */
/* 	 * We believe that rewriting it as a provider will restore this  */
/* 	 * functionality to OSSL 3 but until then we disable it for OSSL 3  */
/* 	 * TODO: Write a provider or figure out another fix.  */
/* 	 *\/ */
/* #if OPENSSL_VERSION_NUMBER <= 0x10100000UL */
/* 	cipher_by_name("aes128-ctr")->evptype = evp_aes_ctr_mt; */
/* 	cipher_by_name("aes192-ctr")->evptype = evp_aes_ctr_mt; */
/* 	cipher_by_name("aes256-ctr")->evptype = evp_aes_ctr_mt; */
/* #endif */
/* } */
/* #endif /\*WITH_OPENSSL*\/ */

u_int
cipher_blocksize(const struct sshcipher *c)
{
	return (c->block_size);
}

u_int
cipher_keylen(const struct sshcipher *c)
{
	return (c->key_len);
}

u_int
cipher_seclen(const struct sshcipher *c)
{
	if (strcmp("3des-cbc", c->name) == 0)
		return 14;
	return cipher_keylen(c);
}

u_int
cipher_authlen(const struct sshcipher *c)
{
	return (c->auth_len);
}

u_int
cipher_ivlen(const struct sshcipher *c)
{
	/*
	 * Default is cipher block size, except for chacha20+poly1305 that
	 * needs no IV. XXX make iv_len == -1 default?
	 */
	return (c->iv_len != 0 || (c->flags & CFLAG_CHACHAPOLY) != 0) ?
	    c->iv_len : c->block_size;
}

u_int
cipher_is_cbc(const struct sshcipher *c)
{
	return (c->flags & CFLAG_CBC) != 0;
}

u_int
cipher_ctx_is_plaintext(struct sshcipher_ctx *cc)
{
	return cc->plaintext;
}

struct sshcipher *
cipher_by_name(const char *name)
{
	struct sshcipher *c;
	for (c = ciphers; c->name != NULL; c++)
		if (strcmp(c->name, name) == 0)
			return c;
	return NULL;
}

#define	CIPHER_SEP	","
int
ciphers_valid(const char *names)
{
	const struct sshcipher *c;
	char *cipher_list, *cp;
	char *p;

	if (names == NULL || strcmp(names, "") == 0)
		return 0;
	if ((cipher_list = cp = strdup(names)) == NULL)
		return 0;
	for ((p = strsep(&cp, CIPHER_SEP)); p && *p != '\0';
	    (p = strsep(&cp, CIPHER_SEP))) {
		c = cipher_by_name(p);
		  if (c == NULL || ((c->flags & CFLAG_INTERNAL) != 0 &&
				    (c->flags & CFLAG_NONE) != 0)) {
			free(cipher_list);
			return 0;
		}
	}
	free(cipher_list);
	return 1;
}

const char *
cipher_warning_message(const struct sshcipher_ctx *cc)
{
	if (cc == NULL || cc->cipher == NULL)
		return NULL;
	/* XXX repurpose for CBC warning */
	return NULL;
}

int
cipher_init(struct sshcipher_ctx **ccp, const struct sshcipher *cipher,
    const u_char *key, u_int keylen, const u_char *iv, u_int ivlen,
	    int do_encrypt, int post_auth)
{
	struct sshcipher_ctx *cc = NULL;
	int ret = SSH_ERR_INTERNAL_ERROR;
#ifdef WITH_OPENSSL
	const EVP_CIPHER *type;
	int klen;
#endif

	*ccp = NULL;
	if ((cc = calloc(sizeof(*cc), 1)) == NULL)
		return SSH_ERR_ALLOC_FAIL;

	cc->plaintext = (cipher->flags & CFLAG_NONE) != 0;
	cc->encrypt = do_encrypt;

	if (keylen < cipher->key_len ||
	    (iv != NULL && ivlen < cipher_ivlen(cipher))) {
		ret = SSH_ERR_INVALID_ARGUMENT;
		goto out;
	}

	cc->cipher = cipher;
	if ((cc->cipher->flags & CFLAG_CHACHAPOLY) != 0) {
		cc->cp_ctx = chachapoly_new(key, keylen);
		ret = cc->cp_ctx != NULL ? 0 : SSH_ERR_INVALID_ARGUMENT;
		goto out;
	}
	if ((cc->cipher->flags & CFLAG_NONE) != 0) {
		ret = 0;
		goto out;
	}
#ifndef WITH_OPENSSL
	if ((cc->cipher->flags & CFLAG_AESCTR) != 0) {
		aesctr_keysetup(&cc->ac_ctx, key, 8 * keylen, 8 * ivlen);
		aesctr_ivsetup(&cc->ac_ctx, iv);
		ret = 0;
		goto out;
	}
	ret = SSH_ERR_INVALID_ARGUMENT;
	goto out;
#else /* WITH_OPENSSL */
	type = (*cipher->evptype)();
	if ((cc->evp = EVP_CIPHER_CTX_new()) == NULL) {
		ret = SSH_ERR_ALLOC_FAIL;
		goto out;
	}
	/* the following block is for AES-CTR-MT cipher switching 
	 * if we are using the ctr cipher and we are post-auth then
	 * start the threaded cipher. If OSSL supports providers (OSSL 3.0+) then
	 * we load our hpnssh provider. If it doesn't (OSSL < 1.1) then we use the
	 * _meth_new process found in cipher-ctr-mt.c */
	if (strstr(cc->cipher->name, "ctr") && post_auth) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000UL
		/* this version of openssl uses providers */
		OSSL_LIB_CTX *aes_lib = NULL; /* probably not needed */
		OSSL_PROVIDER *aes_mt_provider = NULL;
		type = NULL;
		
		if (OSSL_PROVIDER_add_builtin(aes_lib, "hpnssh",
					      OSSL_provider_init) != 1) {
			fatal("Failed to add HPNSSH provider for AES-CTR");
		}
		aes_mt_provider = OSSL_PROVIDER_load(aes_lib, "hpnssh");

		if (aes_mt_provider != NULL) {
			/* use the previous key length to determine which cipher to load */
			if (cipher->key_len == 32)
				type = EVP_CIPHER_fetch(aes_lib, "aes_ctr_mt_256", NULL);
			if (cipher->key_len == 24)
				type = EVP_CIPHER_fetch(aes_lib, "aes_ctr_mt_192", NULL);
			if (cipher->key_len == 16)
				type = EVP_CIPHER_fetch(aes_lib, "aes_ctr_mt_128", NULL);
			if (type == NULL) {
				ERR_print_errors_fp(stderr);
				fatal("FAILED TO LOAD aes_ctr_mt");
			} else {
				debug("LOADED aes_ctr_mt");
			}
		}
		else {
			ERR_print_errors_fp(stderr);
			fatal("Failed to load HPN-SSH AES-CTR-MT provider.");
		}
#else
		/* this version doesn't so check to see if we are in the
		 * LibreSSL hole that doesn't support this at all  otherwise
		 * load the MT cipher */
#if LIBRESSL_VERSION_NUMBER ==  0x3060000fL || LIBRESSL_VERSION_NUMBER ==  0x3050000fL
		fprintf(stderr, "LibreSSL 3.5 & 3.6 do not support the threaded AES CTR cipher.");
		fprintf(stderr, " Falling back to serial.\n");
		type = (*cipher->evptype)();
#else
		type = (*evp_aes_ctr_mt)(); /* see cipher-ctr-mt.c */
#endif
#endif /* OPENSSL_VERSION_NUMBER */
	} /* if (strstr()) */
	if (EVP_CipherInit(cc->evp, type, NULL, (u_char *)iv,
	    (do_encrypt == CIPHER_ENCRYPT)) == 0) {
		ret = SSH_ERR_LIBCRYPTO_ERROR;
		goto out;
	}
	if (cipher_authlen(cipher) &&
	    !EVP_CIPHER_CTX_ctrl(cc->evp, EVP_CTRL_GCM_SET_IV_FIXED,
	    -1, (u_char *)iv)) {
		ret = SSH_ERR_LIBCRYPTO_ERROR;
		goto out;
	}
	klen = EVP_CIPHER_CTX_key_length(cc->evp);
	if (klen > 0 && keylen != (u_int)klen) {
		if (EVP_CIPHER_CTX_set_key_length(cc->evp, keylen) == 0) {
			ret = SSH_ERR_LIBCRYPTO_ERROR;
			goto out;
		}
	}
	if (EVP_CipherInit(cc->evp, NULL, (u_char *)key, NULL, -1) == 0) {
		ret = SSH_ERR_LIBCRYPTO_ERROR;
		goto out;
	}
	ret = 0;
#endif /* WITH_OPENSSL */
 out:
	if (ret == 0) {
		/* success */
		*ccp = cc;
	} else {
		if (cc != NULL) {
#ifdef WITH_OPENSSL
			EVP_CIPHER_CTX_free(cc->evp);
#endif /* WITH_OPENSSL */
			freezero(cc, sizeof(*cc));
		}
	}
	return ret;
}

/*
 * cipher_crypt() operates as following:
 * Copy 'aadlen' bytes (without en/decryption) from 'src' to 'dest'.
 * These bytes are treated as additional authenticated data for
 * authenticated encryption modes.
 * En/Decrypt 'len' bytes at offset 'aadlen' from 'src' to 'dest'.
 * Use 'authlen' bytes at offset 'len'+'aadlen' as the authentication tag.
 * This tag is written on encryption and verified on decryption.
 * Both 'aadlen' and 'authlen' can be set to 0.
 */
int
cipher_crypt(struct sshcipher_ctx *cc, u_int seqnr, u_char *dest,
   const u_char *src, u_int len, u_int aadlen, u_int authlen)
{
	if ((cc->cipher->flags & CFLAG_CHACHAPOLY) != 0) {
		return chachapoly_crypt(cc->cp_ctx, seqnr, dest, src,
		    len, aadlen, authlen, cc->encrypt);
	}
	if ((cc->cipher->flags & CFLAG_NONE) != 0) {
		memcpy(dest, src, aadlen + len);
		return 0;
	}
#ifndef WITH_OPENSSL
	if ((cc->cipher->flags & CFLAG_AESCTR) != 0) {
		if (aadlen)
			memcpy(dest, src, aadlen);
		aesctr_encrypt_bytes(&cc->ac_ctx, src + aadlen,
		    dest + aadlen, len);
		return 0;
	}
	return SSH_ERR_INVALID_ARGUMENT;
#else
	if (authlen) {
		u_char lastiv[1];

		if (authlen != cipher_authlen(cc->cipher))
			return SSH_ERR_INVALID_ARGUMENT;
		/* increment IV */
		if (!EVP_CIPHER_CTX_ctrl(cc->evp, EVP_CTRL_GCM_IV_GEN,
		    1, lastiv))
			return SSH_ERR_LIBCRYPTO_ERROR;
		/* set tag on decyption */
		if (!cc->encrypt &&
		    !EVP_CIPHER_CTX_ctrl(cc->evp, EVP_CTRL_GCM_SET_TAG,
		    authlen, (u_char *)src + aadlen + len))
			return SSH_ERR_LIBCRYPTO_ERROR;
	}
	if (aadlen) {
		if (authlen &&
		    EVP_Cipher(cc->evp, NULL, (u_char *)src, aadlen) < 0)
			return SSH_ERR_LIBCRYPTO_ERROR;
		memcpy(dest, src, aadlen);
	}
	if (len % cc->cipher->block_size)
		return SSH_ERR_INVALID_ARGUMENT;
	if (EVP_Cipher(cc->evp, dest + aadlen, (u_char *)src + aadlen,
	    len) < 0)
		return SSH_ERR_LIBCRYPTO_ERROR;
	if (authlen) {
		/* compute tag (on encrypt) or verify tag (on decrypt) */
		if (EVP_Cipher(cc->evp, NULL, NULL, 0) < 0)
			return cc->encrypt ?
			    SSH_ERR_LIBCRYPTO_ERROR : SSH_ERR_MAC_INVALID;
		if (cc->encrypt &&
		    !EVP_CIPHER_CTX_ctrl(cc->evp, EVP_CTRL_GCM_GET_TAG,
		    authlen, dest + aadlen + len))
			return SSH_ERR_LIBCRYPTO_ERROR;
	}
	return 0;
#endif
}

/* Extract the packet length, including any decryption necessary beforehand */
int
cipher_get_length(struct sshcipher_ctx *cc, u_int *plenp, u_int seqnr,
    const u_char *cp, u_int len)
{
	if ((cc->cipher->flags & CFLAG_CHACHAPOLY) != 0)
		return chachapoly_get_length(cc->cp_ctx, plenp, seqnr,
		    cp, len);
	if (len < 4)
		return SSH_ERR_MESSAGE_INCOMPLETE;
	*plenp = PEEK_U32(cp);
	return 0;
}

void
cipher_free(struct sshcipher_ctx *cc)
{
	if (cc == NULL)
		return;
	if ((cc->cipher->flags & CFLAG_CHACHAPOLY) != 0) {
		chachapoly_free(cc->cp_ctx);
		cc->cp_ctx = NULL;
	} else if ((cc->cipher->flags & CFLAG_AESCTR) != 0)
		explicit_bzero(&cc->ac_ctx, sizeof(cc->ac_ctx));
#ifdef WITH_OPENSSL
	EVP_CIPHER_CTX_free(cc->evp);
	cc->evp = NULL;
#endif
	freezero(cc, sizeof(*cc));
}

/*
 * Exports an IV from the sshcipher_ctx required to export the key
 * state back from the unprivileged child to the privileged parent
 * process.
 */
int
cipher_get_keyiv_len(const struct sshcipher_ctx *cc)
{
	const struct sshcipher *c = cc->cipher;

	if ((c->flags & CFLAG_CHACHAPOLY) != 0)
		return 0;
	else if ((c->flags & CFLAG_AESCTR) != 0)
		return sizeof(cc->ac_ctx.ctr);
#ifdef WITH_OPENSSL
	return EVP_CIPHER_CTX_iv_length(cc->evp);
#else
	return 0;
#endif
}

int
cipher_get_keyiv(struct sshcipher_ctx *cc, u_char *iv, size_t len)
{
#ifdef WITH_OPENSSL
	const struct sshcipher *c = cc->cipher;
	int evplen;
#endif

	if ((cc->cipher->flags & CFLAG_CHACHAPOLY) != 0) {
		if (len != 0)
			return SSH_ERR_INVALID_ARGUMENT;
		return 0;
	}
	if ((cc->cipher->flags & CFLAG_AESCTR) != 0) {
		if (len != sizeof(cc->ac_ctx.ctr))
			return SSH_ERR_INVALID_ARGUMENT;
		memcpy(iv, cc->ac_ctx.ctr, len);
		return 0;
	}
	if ((cc->cipher->flags & CFLAG_NONE) != 0)
		return 0;

#ifdef WITH_OPENSSL
	evplen = EVP_CIPHER_CTX_iv_length(cc->evp);
	if (evplen == 0)
		return 0;
	else if (evplen < 0)
		return SSH_ERR_LIBCRYPTO_ERROR;
	if ((size_t)evplen != len)
		return SSH_ERR_INVALID_ARGUMENT;
	if (cipher_authlen(c)) {
		if (!EVP_CIPHER_CTX_ctrl(cc->evp, EVP_CTRL_GCM_IV_GEN,
		    len, iv))
			return SSH_ERR_LIBCRYPTO_ERROR;
	} else if (!EVP_CIPHER_CTX_get_iv(cc->evp, iv, len))
		return SSH_ERR_LIBCRYPTO_ERROR;
#endif
	return 0;
}

int
cipher_set_keyiv(struct sshcipher_ctx *cc, const u_char *iv, size_t len)
{
#ifdef WITH_OPENSSL
	const struct sshcipher *c = cc->cipher;
	int evplen = 0;
#endif

	if ((cc->cipher->flags & CFLAG_CHACHAPOLY) != 0)
		return 0;
	if ((cc->cipher->flags & CFLAG_NONE) != 0)
		return 0;

#ifdef WITH_OPENSSL
	evplen = EVP_CIPHER_CTX_iv_length(cc->evp);
	if (evplen <= 0)
		return SSH_ERR_LIBCRYPTO_ERROR;
	if ((size_t)evplen != len)
		return SSH_ERR_INVALID_ARGUMENT;
	if (cipher_authlen(c)) {
		/* XXX iv arg is const, but EVP_CIPHER_CTX_ctrl isn't */
		if (!EVP_CIPHER_CTX_ctrl(cc->evp,
		    EVP_CTRL_GCM_SET_IV_FIXED, -1, (void *)iv))
			return SSH_ERR_LIBCRYPTO_ERROR;
	} else if (!EVP_CIPHER_CTX_set_iv(cc->evp, iv, evplen))
		return SSH_ERR_LIBCRYPTO_ERROR;
#endif
	return 0;
}
