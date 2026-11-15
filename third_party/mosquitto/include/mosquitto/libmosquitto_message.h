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

#ifndef MOSQUITTO_LIBMOSQUITTO_MESSAGE_H
#define MOSQUITTO_LIBMOSQUITTO_MESSAGE_H

/*
 * File: mosquitto/libmosquitto_message.h
 *
 * This header contains functions for handling mosquitto_message structs.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 *
 * Section: Struct mosquitto_message helper functions
 *
 * ====================================================================== */
/*
 * Function: mosquitto_message_copy
 *
 * Copy the contents of a mosquitto message to another message.
 * Useful for preserving a message received in the on_message() callback.
 *
 * Parameters:
 *	dst - a pointer to a valid mosquitto_message struct to copy to.
 *	src - a pointer to a valid mosquitto_message struct to copy from.
 *
 * Returns:
 *	MOSQ_ERR_SUCCESS - on success.
 * 	MOSQ_ERR_INVAL -   if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -   if an out of memory condition occurred.
 *
 * See Also:
 * 	<mosquitto_message_free>
 */
libmosq_EXPORT int mosquitto_message_copy(struct mosquitto_message *dst, const struct mosquitto_message *src);

/*
 * Function: mosquitto_message_free
 *
 * Completely free a mosquitto_message struct.
 *
 * Parameters:
 *	message - pointer to a mosquitto_message pointer to free.
 *
 * See Also:
 * 	<mosquitto_message_copy>, <mosquitto_message_free_contents>
 */
libmosq_EXPORT void mosquitto_message_free(struct mosquitto_message **message);

/*
 * Function: mosquitto_message_free_contents
 *
 * Free a mosquitto_message struct contents, leaving the struct unaffected.
 *
 * Parameters:
 *	message - pointer to a mosquitto_message struct to free its contents.
 *
 * See Also:
 * 	<mosquitto_message_copy>, <mosquitto_message_free>
 */
libmosq_EXPORT void mosquitto_message_free_contents(struct mosquitto_message *message);

#ifdef __cplusplus
}
#endif

#endif
