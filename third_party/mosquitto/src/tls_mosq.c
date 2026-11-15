/*
Copyright (c) 2013-2021 Roger Light <roger@atchoo.org>

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

#include "config.h"

#ifdef WITH_TLS

#ifdef WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <sys/socket.h>
#  include <strings.h>
#endif

#include <string.h>
#include <openssl/conf.h>
#include <openssl/x509v3.h>
#include <openssl/ssl.h>

#ifdef WITH_BROKER
#  include "mosquitto_broker_internal.h"
#endif
#include "mosquitto_internal.h"
#include "logging_mosq.h"
#include "tls_mosq.h"


int mosquitto__server_certificate_verify(int preverify_ok, X509_STORE_CTX *ctx)
{
	UNUSED(ctx);

	return preverify_ok;
}


int tls__set_verify_hostname(struct mosquitto *mosq, const char *hostname)
{
	unsigned char ipv6_addr[16];
	unsigned char ipv4_addr[4];
	int ipv6_ok;
	int ipv4_ok;
	int rc;

	if(mosq->tls_insecure == true
			|| (mosq->tls_cafile == NULL && mosq->tls_capath == NULL && mosq->tls_use_os_certs == false)){

		return MOSQ_ERR_SUCCESS;
	}
#ifndef WITH_BROKER
	if(mosq->port == 0){
		/* No hostname verification for unix sockets */
		return MOSQ_ERR_SUCCESS;
	}
#endif
#ifdef WIN32
	ipv6_ok = InetPton(AF_INET6, hostname, &ipv6_addr);
	ipv4_ok = InetPton(AF_INET, hostname, &ipv4_addr);
#else
	ipv6_ok = inet_pton(AF_INET6, hostname, &ipv6_addr);
	ipv4_ok = inet_pton(AF_INET, hostname, &ipv4_addr);
#endif

	X509_VERIFY_PARAM *param = SSL_get0_param(mosq->ssl);
	if(ipv4_ok || ipv6_ok){
		rc = X509_VERIFY_PARAM_set1_ip_asc(param, hostname);
	}else{
		rc = X509_VERIFY_PARAM_set1_host(param, hostname, 0);
	}
	if(rc == 1){
		return MOSQ_ERR_SUCCESS;
	}else{
		return MOSQ_ERR_TLS;
	}
}
#endif
