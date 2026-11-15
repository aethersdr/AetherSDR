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

#ifndef MOSQUITTO_LIBMOSQUITTO_SOCKS_H
#define MOSQUITTO_LIBMOSQUITTO_SOCKS_H

/*
 * File: mosquitto/libmosquitto_socks.h
 *
 * This header contains functions for controlling SOCKSv5 in libmosquitto.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 *
 * Section: SOCKS5 proxy functions
 *
 * =============================================================================
 */

/*
 * Function: mosquitto_socks5_set
 *
 * Configure the client to use a SOCKS5 proxy when connecting. Must be called
 * before connecting. "None" and "username/password" authentication is
 * supported.
 *
 * Parameters:
 *   mosq - a valid mosquitto instance.
 *   host - the SOCKS5 proxy host to connect to.
 *   port - the SOCKS5 proxy port to use.
 *   username - if not NULL, use this username when authenticating with the proxy.
 *   password - if not NULL and username is not NULL, use this password when
 *              authenticating with the proxy.
 */
libmosq_EXPORT int mosquitto_socks5_set(struct mosquitto *mosq, const char *host, int port, const char *username, const char *password);

#ifdef __cplusplus
}
#endif

#endif
