/*	$Id$ */
/*
 * Copyright (c) 2016 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHORS DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <sys/stat.h>
#include <sys/param.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
# include <sandbox.h>
#endif

#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/engine.h>

#include "extern.h"

#define	CERT_PEM "cert.pem"
#define	RENEW_ALLOW (30 * 24 * 60 * 60)

/*
 * Convert the X509's expiration time (which is in ASN1_TIME format)
 * into a time_t value.
 * There are lots of suggestions on the Internet on how to do this and
 * they're really, really unsafe.
 * Adapt those poor solutions to a safe one.
 */
static time_t
X509expires(X509 *x)
{
	ASN1_TIME	*time;
	struct tm	 t;
	unsigned char	*str;
	size_t 	 	 i = 0;

	time = X509_get_notAfter(x);
	str = time->data;
	memset(&t, 0, sizeof(t));

	/* Account for 2 and 4-digit time. */

	if (time->type == V_ASN1_UTCTIME) {
		if (time->length <= 2) {
			dowarnx("invalid ASN1_TIME");
			return((time_t)-1);
		}
		t.tm_year = 
			(str[0] - '0') * 10 + 
			(str[1] - '0');
		if (t.tm_year < 70)
			t.tm_year += 100;
		i = 2;
	} else if (time->type == V_ASN1_GENERALIZEDTIME) {
		if (time->length <= 4) {
			dowarnx("invalid ASN1_TIME");
			return((time_t)-1);
		}
		t.tm_year = 
			(str[0] - '0') * 1000 + 
			(str[1] - '0') * 100 + 
			(str[2] - '0') * 10 + 
			(str[3] - '0');
		t.tm_year -= 1900;
		i = 4;
	}

	/* Now the post-year parts. */

	if (time->length <= (int)i + 10) {
		dowarnx("invalid ASN1_TIME");
		return((time_t)-1);
	}

	t.tm_mon = ((str[i + 0] - '0') * 10 + (str[i + 1] - '0')) - 1;
	t.tm_mday = (str[i + 2] - '0') * 10 + (str[i + 3] - '0');
	t.tm_hour = (str[i + 4] - '0') * 10 + (str[i + 5] - '0');
	t.tm_min  = (str[i + 6] - '0') * 10 + (str[i + 7] - '0');
	t.tm_sec  = (str[i + 8] - '0') * 10 + (str[i + 9] - '0');
	
	return(mktime(&t));
}

int
revokeproc(int netsock, const char *certdir, 
	uid_t uid, gid_t gid, int force)
{
	int		 rc;
	long		 lval;
	FILE		*f;
	char		*path;
	X509		*x;
	enum revokeop	 op, rop;
	time_t		 t;

	rc = 0;
	f = NULL;
	path = NULL;
	x = NULL;

	/*
	 * First try to open the certificate before we drop privileges
	 * and jail ourselves.
	 * We allow "f" to be NULL IFF the cert doesn't exist yet.
	 */

	if (-1 == asprintf(&path, "%s/%s", certdir, CERT_PEM)) {
		dowarn("asprintf");
		goto out;
	} else if (NULL == (f = fopen(path, "r")) && ENOENT != errno) {
		dowarn("%s", path);
		goto out;
	}

	/* File-system and sandbox jailing. */

#ifdef __APPLE__
	if (-1 == sandbox_init(kSBXProfileNoNetwork, 
 	    SANDBOX_NAMED, NULL)) {
		dowarn("sandbox_init");
		goto out;
	}
#endif
	ERR_load_crypto_strings();

	if ( ! dropfs(PATH_VAR_EMPTY)) {
		dowarnx("dropfs");
		goto out;
	} else if ( ! dropprivs(uid, gid)) {
		dowarnx("dropprivs");
		goto out;

	}
#if defined(__OpenBSD__) && OpenBSD >= 201605
	if (-1 == pledge("stdio", NULL)) {
		dowarn("pledge");
		goto out;
	}
#endif

	/*
	 * If we couldn't open the certificate, it doesn't exist so we
	 * haven't submitted it yet, so obviously we can mark that it
	 * has expired and we should renew it.
	 */

	if (NULL == f) {
		if (writeop(netsock, COMM_REVOKE_RESP, REVOKE_EXP))
			rc = 1;
		goto out;
	} 

	/* Read out the expiration date. */
	
	if (NULL == (x = PEM_read_X509(f, NULL, NULL, NULL))) {
		dowarnx("PEM_read_X509");
		goto out;
	} else if ((time_t)-1 == (t = X509expires(x))) {
		dowarnx("X509expires");
		goto out;
	}

	rop = time(NULL) >= (t - RENEW_ALLOW) ? REVOKE_EXP : REVOKE_OK;

	if (REVOKE_EXP == rop)
		dodbg("%s/%s: certificate renewable: %lld days left",
			certdir, CERT_PEM, 
			(long long)(t - time(NULL)) / 24 / 60 / 60);
	else
		dodbg("%s/%s: certificate valid: %lld days left",
			certdir, CERT_PEM, 
			(long long)(t - time(NULL)) / 24 / 60 / 60);

	if (REVOKE_OK == rop && force) {
		dowarnx("%s/%s: forcing renewal", certdir, CERT_PEM);
		rop = REVOKE_EXP;
	}

	/* We can re-submit it given RENEW_ALLOW time before. */

	if ( ! writeop(netsock, COMM_REVOKE_RESP, rop))
		goto out;

	op = REVOKE__MAX;
	if (0 == (lval = readop(netsock, COMM_REVOKE_OP)))
		op = REVOKE_STOP;
	else if (REVOKE_CHECK == lval)
		op = lval;

	if (REVOKE__MAX == op) {
		dowarnx("unknown operation from netproc");
		goto out;
	} else if (REVOKE_STOP == op) {
		rc = 1;
		goto out;
	}

	/* TODO: if asking for cert, return it for revocation. */

	rc = 1;
out:
	close(netsock);
	if (NULL != f)
		fclose(f);
	if (NULL != x)
		X509_free(x);
	free(path);
	ERR_print_errors_fp(stderr);
	ERR_free_strings();
	return(rc);
}
