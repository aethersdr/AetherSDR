/*
Copyright (c) 2010-2021 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License 2.0
and Eclipse Distribution License v1.0 which accompany this distribution.

The Eclipse Public License is available at
   https://www.eclipse.org/legal/epl-2.0/
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.

SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

Contributors:
   Roger Light - initial implementation and documentation.
*/

#ifndef MOSQUITTO_LIBMOSQUITTO_TLS_H
#define MOSQUITTO_LIBMOSQUITTO_TLS_H

/*
 * File: mosquitto/libmosquitto_tls.h
 *
 * This header contains functions for setting TLS options in libmosquitto.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 *
 * Section: TLS support
 *
 * ====================================================================== */
/*
 * Function: mosquitto_tls_set
 *
 * Configure the client for certificate based SSL/TLS support. Must be called
 * before <mosquitto_connect>.
 *
 * Cannot be used in conjunction with <mosquitto_tls_psk_set>.
 *
 * Define the Certificate Authority certificates to be trusted (ie. the server
 * certificate must be signed with one of these certificates) using cafile.
 *
 * If the server you are connecting to requires clients to provide a
 * certificate, define certfile and keyfile with your client certificate and
 * private key. If your private key is encrypted, provide a password callback
 * function or you will have to enter the password at the command line.
 *
 * Parameters:
 *  mosq -        a valid mosquitto instance.
 *  cafile -      path to a file containing the PEM encoded trusted CA
 *                certificate files. Either cafile or capath must not be NULL.
 *  capath -      path to a directory containing the PEM encoded trusted CA
 *                certificate files. See mosquitto.conf for more details on
 *                configuring this directory. Either cafile or capath must not
 *                be NULL.
 *  certfile -    path to a file containing the PEM encoded certificate file
 *                for this client. If NULL, keyfile must also be NULL and no
 *                client certificate will be used.
 *  keyfile -     path to a file containing the PEM encoded private key for
 *                this client. If NULL, certfile must also be NULL and no
 *                client certificate will be used.
 *  pw_callback - if keyfile is encrypted, set pw_callback to allow your client
 *                to pass the correct password for decryption. If set to NULL,
 *                the password must be entered on the command line.
 *                Your callback must write the password into "buf", which is
 *                "size" bytes long. The return value must be the length of the
 *                password. "userdata" will be set to the calling mosquitto
 *                instance. The mosquitto userdata member variable can be
 *                retrieved using <mosquitto_userdata>.
 *
 * Returns:
 *	MOSQ_ERR_SUCCESS - on success.
 * 	MOSQ_ERR_INVAL -   if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -   if an out of memory condition occurred.
 *
 * See Also:
 *	<mosquitto_tls_opts_set>, <mosquitto_tls_psk_set>,
 *	<mosquitto_tls_insecure_set>, <mosquitto_userdata>
 */
libmosq_EXPORT int mosquitto_tls_set(struct mosquitto *mosq,
		const char *cafile, const char *capath,
		const char *certfile, const char *keyfile,
		int (*pw_callback)(char *buf, int size, int rwflag, void *userdata));

/*
 * Function: mosquitto_tls_insecure_set
 *
 * Configure verification of the server hostname in the server certificate. If
 * value is set to true, it is impossible to guarantee that the host you are
 * connecting to is not impersonating your server. This can be useful in
 * initial server testing, but makes it possible for a malicious third party to
 * impersonate your server through DNS spoofing, for example.
 * Do not use this function in a real system. Setting value to true makes the
 * connection encryption pointless.
 * Must be called before <mosquitto_connect>.
 *
 * Parameters:
 *  mosq -  a valid mosquitto instance.
 *  value - if set to false, the default, certificate hostname checking is
 *          performed. If set to true, no hostname checking is performed and
 *          the connection is insecure.
 *
 * Returns:
 *	MOSQ_ERR_SUCCESS - on success.
 * 	MOSQ_ERR_INVAL -   if the input parameters were invalid.
 *
 * See Also:
 *	<mosquitto_tls_set>
 */
libmosq_EXPORT int mosquitto_tls_insecure_set(struct mosquitto *mosq, bool value);

/*
 * Function: mosquitto_tls_opts_set
 *
 * Set advanced SSL/TLS options. Must be called before <mosquitto_connect>.
 *
 * Parameters:
 *  mosq -        a valid mosquitto instance.
 *	cert_reqs -   an integer defining the verification requirements the client
 *	              will impose on the server. This can be one of:
 *	              * SSL_VERIFY_NONE (0): the server will not be verified in any way.
 *	              * SSL_VERIFY_PEER (1): the server certificate will be verified
 *	                and the connection aborted if the verification fails.
 *	              The default and recommended value is SSL_VERIFY_PEER. Using
 *	              SSL_VERIFY_NONE provides no security.
 *	tls_version - the version of the SSL/TLS protocol to use as a string. If NULL,
 *	              the default value is used. The default value and the
 *	              available values depend on the version of openssl that the
 *	              library was compiled against. The available options are
 *	              tlsv1.3 and tlsv1.2, with tlsv1.2 as the default.
 *	ciphers -     a string describing the ciphers available for use. See the
 *	              "openssl ciphers" tool for more information. If NULL, the
 *	              default ciphers will be used.
 *
 * Returns:
 *	MOSQ_ERR_SUCCESS - on success.
 * 	MOSQ_ERR_INVAL -   if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -   if an out of memory condition occurred.
 *
 * See Also:
 *	<mosquitto_tls_set>
 */
libmosq_EXPORT int mosquitto_tls_opts_set(struct mosquitto *mosq, int cert_reqs, const char *tls_version, const char *ciphers);

/*
 * Function: mosquitto_tls_psk_set
 *
 * Configure the client for pre-shared-key based TLS support. Must be called
 * before <mosquitto_connect>.
 *
 * Cannot be used in conjunction with <mosquitto_tls_set>.
 *
 * Parameters:
 *  mosq -     a valid mosquitto instance.
 *  psk -      the pre-shared-key in hex format with no leading "0x".
 *  identity - the identity of this client. May be used as the username
 *             depending on the server settings.
 *	ciphers -  a string describing the PSK ciphers available for use. See the
 *	           "openssl ciphers" tool for more information. If NULL, the
 *	           default ciphers will be used.
 *
 * Returns:
 *	MOSQ_ERR_SUCCESS - on success.
 * 	MOSQ_ERR_INVAL -   if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -   if an out of memory condition occurred.
 *
 * See Also:
 *	<mosquitto_tls_set>
 */
libmosq_EXPORT int mosquitto_tls_psk_set(struct mosquitto *mosq, const char *psk, const char *identity, const char *ciphers);


/*
 * Function: mosquitto_ssl_get
 *
 * Retrieve a pointer to the SSL structure used for TLS connections in this
 * client. This can be used in e.g. the connect callback to carry out
 * additional verification steps.
 *
 * Parameters:
 *  mosq - a valid mosquitto instance
 *
 * Returns:
 *  A valid pointer to an openssl SSL structure - if the client is using TLS.
 *  NULL - if the client is not using TLS, or TLS support is not compiled in.
 */
libmosq_EXPORT void *mosquitto_ssl_get(struct mosquitto *mosq);

#ifdef __cplusplus
}
#endif

#endif
