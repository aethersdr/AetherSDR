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

#ifndef MOSQUITTO_LIBMOSQUITTO_CREATE_DELETE_H
#define MOSQUITTO_LIBMOSQUITTO_CREATE_DELETE_H

/*
 * File: mosquitto/libmosquitto_create_delete.h
 *
 * This header contains functions for creating/deleting/reinitialising mosquitto clients.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 *
 * Section: Client creation, destruction, and reinitialisation
 *
 * ====================================================================== */
/*
 * Function: mosquitto_new
 *
 * Create a new mosquitto client instance.
 *
 * Parameters:
 * 	id -            String to use as the client id. If NULL, a random client id
 * 	                will be generated. If id is NULL, clean_session must be true.
 * 	clean_session - set to true to instruct the broker to clean all messages
 *                  and subscriptions on disconnect, false to instruct it to
 *                  keep them. See the man page mqtt(7) for more details.
 *                  Note that a client will never discard its own outgoing
 *                  messages on disconnect. Calling <mosquitto_connect> or
 *                  <mosquitto_reconnect> will cause the messages to be resent.
 *                  Use <mosquitto_reinitialise> to reset a client to its
 *                  original state.
 *                  Must be set to true if the id parameter is NULL.
 * 	obj -           A user pointer that will be passed as an argument to any
 *                  callbacks that are specified.
 *
 * Returns:
 * 	Pointer to a struct mosquitto on success.
 * 	NULL on failure. Interrogate errno to determine the cause for the failure:
 *      - ENOMEM on out of memory.
 *      - EINVAL on invalid input parameters.
 *
 * See Also:
 * 	<mosquitto_reinitialise>, <mosquitto_destroy>, <mosquitto_user_data_set>
 */
libmosq_EXPORT struct mosquitto *mosquitto_new(const char *id, bool clean_session, void *obj);

/*
 * Function: mosquitto_destroy
 *
 * Use to free memory associated with a mosquitto client instance.
 *
 * Parameters:
 * 	mosq - a struct mosquitto pointer to free.
 *
 * See Also:
 * 	<mosquitto_new>, <mosquitto_reinitialise>
 */
libmosq_EXPORT void mosquitto_destroy(struct mosquitto *mosq);

/*
 * Function: mosquitto_reinitialise
 *
 * This function allows an existing mosquitto client to be reused. Call on a
 * mosquitto instance to close any open network connections, free memory
 * and reinitialise the client with the new parameters. The end result is the
 * same as the output of <mosquitto_new>.
 *
 * Parameters:
 * 	mosq -          a valid mosquitto instance.
 * 	id -            string to use as the client id. If NULL, a random client id
 * 	                will be generated. If id is NULL, clean_session must be true.
 * 	clean_session - set to true to instruct the broker to clean all messages
 *                  and subscriptions on disconnect, false to instruct it to
 *                  keep them. See the man page mqtt(7) for more details.
 *                  Must be set to true if the id parameter is NULL.
 * 	obj -           A user pointer that will be passed as an argument to any
 *                  callbacks that are specified.
 *
 * Returns:
 * 	MOSQ_ERR_SUCCESS -        on success.
 * 	MOSQ_ERR_INVAL -          if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -          if an out of memory condition occurred.
 * 	MOSQ_ERR_MALFORMED_UTF8 - if the client id is not valid UTF-8.
 *
 * See Also:
 * 	<mosquitto_new>, <mosquitto_destroy>
 */
libmosq_EXPORT int mosquitto_reinitialise(struct mosquitto *mosq, const char *id, bool clean_session, void *obj);

#ifdef __cplusplus
}
#endif

#endif
