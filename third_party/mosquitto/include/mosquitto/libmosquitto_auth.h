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

#ifndef MOSQUITTO_LIBMOSQUITTO_AUTH_H
#define MOSQUITTO_LIBMOSQUITTO_AUTH_H

/*
 * File: mosquitto/libmosquitto_auth.h
 *
 * This header contains functions for setting client authentication parameters in libmosquitto.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 *
 * Section: Username and password
 *
 * ====================================================================== */
/*
 * Function: mosquitto_username_pw_set
 *
 * Configure username and password for a mosquitto instance. By default, no
 * username or password will be sent. For v3.1 and v3.1.1 clients, if username
 * is NULL, the password argument is ignored.
 *
 * This is must be called before calling <mosquitto_connect>.
 *
 * Parameters:
 * 	mosq -     a valid mosquitto instance.
 * 	username - the username to send as a string, or NULL to disable
 *             authentication.
 * 	password - the password to send as a string. Set to NULL when username is
 * 	           valid in order to send just a username.
 *
 * Returns:
 * 	MOSQ_ERR_SUCCESS - on success.
 * 	MOSQ_ERR_INVAL -   if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -   if an out of memory condition occurred.
 */
libmosq_EXPORT int mosquitto_username_pw_set(struct mosquitto *mosq, const char *username, const char *password);

/*
 * Function: mosquitto_ext_auth_continue
 *
 * Use within an on_ext_auth callback only.
 *
 * Call to continue the MQTT v5 extended authentication flow.
 *
 * Parameters:
 * 	mosq -          a valid mosquitto instance.
 * 	auth_method -   the authentication method as provided in the on_ext_auth callback
 * 	auth_data -     authentication data to send to the broker, or NULL
 * 	auth_data_len - the length of auth_data, in bytes, or 0
 *
 * Returns:
 * 	MOSQ_ERR_SUCCESS - on success.
 * 	MOSQ_ERR_INVAL -   if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -   if an out of memory condition occurred.
 */
libmosq_EXPORT int mosquitto_ext_auth_continue(struct mosquitto *context, const char *auth_method, uint16_t auth_data_len, const void *auth_data, const mosquitto_property *props);

#ifdef __cplusplus
}
#endif

#endif
