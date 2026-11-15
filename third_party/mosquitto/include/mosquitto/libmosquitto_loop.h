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

#ifndef MOSQUITTO_LIBMOSQUITTO_LOOP_H
#define MOSQUITTO_LIBMOSQUITTO_LOOP_H

/*
 * File: mosquitto/libmosquitto_loop.h
 *
 * This header contains functions for handling the libmosquitto network loop.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include <mosquitto/defs.h>
#include <mosquitto/mqtt_protocol.h>

/* ======================================================================
 *
 * Section: Network loop (managed by libmosquitto)
 *
 * The internal network loop must be called at a regular interval. The two
 * recommended approaches are to use either <mosquitto_loop_forever> or
 * <mosquitto_loop_start>. <mosquitto_loop_forever> is a blocking call and is
 * suitable for the situation where you only want to handle incoming messages
 * in callbacks. <mosquitto_loop_start> is a non-blocking call, it creates a
 * separate thread to run the loop for you. Use this function when you have
 * other tasks you need to run at the same time as the MQTT client, e.g.
 * reading data from a sensor.
 *
 * ====================================================================== */

/*
 * Function: mosquitto_loop_forever
 *
 * This function call loop() for you in an infinite blocking loop. It is useful
 * for the case where you only want to run the MQTT client loop in your
 * program.
 *
 * It handles reconnecting in case server connection is lost. If you call
 * mosquitto_disconnect() in a callback it will return.
 *
 * Parameters:
 *  mosq - a valid mosquitto instance.
 *	timeout -     Maximum number of milliseconds to wait for network activity
 *	              in the select() call before timing out. Set to 0 for instant
 *	              return.  Set negative to use the default of 1000ms.
 *	max_packets - this parameter is currently unused and should be set to 1 for
 *	              future compatibility.
 *
 * Returns:
 *	MOSQ_ERR_SUCCESS -   on success.
 * 	MOSQ_ERR_INVAL -     if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -     if an out of memory condition occurred.
 * 	MOSQ_ERR_NO_CONN -   if the client isn't connected to a broker.
 *  MOSQ_ERR_CONN_LOST - if the connection to the broker was lost.
 *	MOSQ_ERR_PROTOCOL -  if there is a protocol error communicating with the
 *                       broker.
 * 	MOSQ_ERR_ERRNO -     if a system call returned an error. The variable errno
 *                       contains the error code, even on Windows.
 *                       Use strerror_r() where available or FormatMessage() on
 *                       Windows.
 *
 * See Also:
 *	<mosquitto_loop>, <mosquitto_loop_start>
 */
libmosq_EXPORT int mosquitto_loop_forever(struct mosquitto *mosq, int timeout, int max_packets);

/*
 * Function: mosquitto_loop_start
 *
 * This is part of the threaded client interface. Call this once to start a new
 * thread to process network traffic. This provides an alternative to
 * repeatedly calling <mosquitto_loop> yourself.
 *
 * Parameters:
 *  mosq - a valid mosquitto instance.
 *
 * Returns:
 *	MOSQ_ERR_SUCCESS -       on success.
 * 	MOSQ_ERR_INVAL -         if the input parameters were invalid.
 *	MOSQ_ERR_NOT_SUPPORTED - if thread support is not available.
 *
 * See Also:
 *	<mosquitto_connect_async>, <mosquitto_loop>, <mosquitto_loop_forever>, <mosquitto_loop_stop>
 */
libmosq_EXPORT int mosquitto_loop_start(struct mosquitto *mosq);

/*
 * Function: mosquitto_loop_stop
 *
 * This is part of the threaded client interface. Call this once to stop the
 * network thread previously created with <mosquitto_loop_start>. This call
 * will block until the network thread finishes. For the network thread to end,
 * you must have previously called <mosquitto_disconnect> or have set the force
 * parameter to true.
 *
 * Parameters:
 *  mosq - a valid mosquitto instance.
 *	force - set to true to force thread cancellation. If false,
 *	        <mosquitto_disconnect> must have already been called.
 *
 * Returns:
 *	MOSQ_ERR_SUCCESS -       on success.
 * 	MOSQ_ERR_INVAL -         if the input parameters were invalid.
 *	MOSQ_ERR_NOT_SUPPORTED - if thread support is not available.
 *
 * See Also:
 *	<mosquitto_loop>, <mosquitto_loop_start>
 */
libmosq_EXPORT int mosquitto_loop_stop(struct mosquitto *mosq, bool force);

/*
 * Function: mosquitto_loop
 *
 * The main network loop for the client. This must be called frequently
 * to keep communications between the client and broker working. This is
 * carried out by <mosquitto_loop_forever> and <mosquitto_loop_start>, which
 * are the recommended ways of handling the network loop. You may also use this
 * function if you wish. It must not be called inside a callback.
 *
 * If incoming data is present it will then be processed. Outgoing commands,
 * from e.g.  <mosquitto_publish>, are normally sent immediately that their
 * function is called, but this is not always possible. <mosquitto_loop> will
 * also attempt to send any remaining outgoing messages, which also includes
 * commands that are part of the flow for messages with QoS>0.
 *
 * This calls select() to monitor the client network socket. If you want to
 * integrate mosquitto client operation with your own select() call, use
 * <mosquitto_socket>, <mosquitto_loop_read>, <mosquitto_loop_write> and
 * <mosquitto_loop_misc>.
 *
 * Threads:
 *
 * Parameters:
 *	mosq -        a valid mosquitto instance.
 *	timeout -     Maximum number of milliseconds to wait for network activity
 *	              in the select() call before timing out. Set to 0 for instant
 *	              return.  Set negative to use the default of 1000ms.
 *	max_packets - this parameter is currently unused and should be set to 1 for
 *	              future compatibility.
 *
 * Returns:
 *	MOSQ_ERR_SUCCESS -   on success.
 * 	MOSQ_ERR_INVAL -     if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -     if an out of memory condition occurred.
 * 	MOSQ_ERR_NO_CONN -   if the client isn't connected to a broker.
 *  MOSQ_ERR_CONN_LOST - if the connection to the broker was lost.
 *	MOSQ_ERR_PROTOCOL -  if there is a protocol error communicating with the
 *                       broker.
 * 	MOSQ_ERR_ERRNO -     if a system call returned an error. The variable errno
 *                       contains the error code, even on Windows.
 *                       Use strerror_r() where available or FormatMessage() on
 *                       Windows.
 * See Also:
 *	<mosquitto_loop_forever>, <mosquitto_loop_start>, <mosquitto_loop_stop>
 */
libmosq_EXPORT int mosquitto_loop(struct mosquitto *mosq, int timeout, int max_packets);

/* ======================================================================
 *
 * Section: Network loop (for use in other event loops)
 *
 * ====================================================================== */
/*
 * Function: mosquitto_loop_read
 *
 * Carry out network read operations.
 * This should only be used if you are not using mosquitto_loop() and are
 * monitoring the client network socket for activity yourself.
 *
 * Parameters:
 *	mosq -        a valid mosquitto instance.
 *	max_packets - this parameter is currently unused and should be set to 1 for
 *	              future compatibility.
 *
 * Returns:
 *	MOSQ_ERR_SUCCESS -   on success.
 * 	MOSQ_ERR_INVAL -     if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -     if an out of memory condition occurred.
 * 	MOSQ_ERR_NO_CONN -   if the client isn't connected to a broker.
 *  MOSQ_ERR_CONN_LOST - if the connection to the broker was lost.
 *	MOSQ_ERR_PROTOCOL -  if there is a protocol error communicating with the
 *                       broker.
 * 	MOSQ_ERR_ERRNO -     if a system call returned an error. The variable errno
 *                       contains the error code, even on Windows.
 *                       Use strerror_r() where available or FormatMessage() on
 *                       Windows.
 *
 * See Also:
 *	<mosquitto_socket>, <mosquitto_loop_write>, <mosquitto_loop_misc>
 */
libmosq_EXPORT int mosquitto_loop_read(struct mosquitto *mosq, int max_packets);

/*
 * Function: mosquitto_loop_write
 *
 * Carry out network write operations.
 * This should only be used if you are not using mosquitto_loop() and are
 * monitoring the client network socket for activity yourself.
 *
 * Parameters:
 *	mosq -        a valid mosquitto instance.
 *	max_packets - this parameter is currently unused and should be set to 1 for
 *	              future compatibility.
 *
 * Returns:
 *	MOSQ_ERR_SUCCESS -   on success.
 * 	MOSQ_ERR_INVAL -     if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -     if an out of memory condition occurred.
 * 	MOSQ_ERR_NO_CONN -   if the client isn't connected to a broker.
 *  MOSQ_ERR_CONN_LOST - if the connection to the broker was lost.
 *	MOSQ_ERR_PROTOCOL -  if there is a protocol error communicating with the
 *                       broker.
 * 	MOSQ_ERR_ERRNO -     if a system call returned an error. The variable errno
 *                       contains the error code, even on Windows.
 *                       Use strerror_r() where available or FormatMessage() on
 *                       Windows.
 *
 * See Also:
 *	<mosquitto_socket>, <mosquitto_loop_read>, <mosquitto_loop_misc>, <mosquitto_want_write>
 */
libmosq_EXPORT int mosquitto_loop_write(struct mosquitto *mosq, int max_packets);

/*
 * Function: mosquitto_loop_misc
 *
 * Carry out miscellaneous operations required as part of the network loop.
 * This should only be used if you are not using mosquitto_loop() and are
 * monitoring the client network socket for activity yourself.
 *
 * This function deals with handling PINGs and checking whether messages need
 * to be retried, so should be called fairly frequently, around once per second
 * is sufficient.
 *
 * Parameters:
 *	mosq - a valid mosquitto instance.
 *
 * Returns:
 *	MOSQ_ERR_SUCCESS -   on success.
 * 	MOSQ_ERR_INVAL -     if the input parameters were invalid.
 * 	MOSQ_ERR_NO_CONN -   if the client isn't connected to a broker.
 *
 * See Also:
 *	<mosquitto_socket>, <mosquitto_loop_read>, <mosquitto_loop_write>
 */
libmosq_EXPORT int mosquitto_loop_misc(struct mosquitto *mosq);


/* ======================================================================
 *
 * Section: Network loop (helper functions)
 *
 * ====================================================================== */
/*
 * Function: mosquitto_socket
 *
 * Return the socket handle for a mosquitto instance. Useful if you want to
 * include a mosquitto client in your own select() calls.
 *
 * Parameters:
 *	mosq - a valid mosquitto instance.
 *
 * Returns:
 *	The socket for the mosquitto client or -1 on failure.
 */
libmosq_EXPORT int mosquitto_socket(struct mosquitto *mosq);

/*
 * Function: mosquitto_want_write
 *
 * Returns true if there is data ready to be written on the socket.
 *
 * Parameters:
 *	mosq - a valid mosquitto instance.
 *
 * See Also:
 *	<mosquitto_socket>, <mosquitto_loop_read>, <mosquitto_loop_write>
 */
libmosq_EXPORT bool mosquitto_want_write(struct mosquitto *mosq);

/*
 * Function: mosquitto_threaded_set
 *
 * Used to tell the library that your application is using threads, but not
 * using <mosquitto_loop_start>. The library operates slightly differently when
 * not in threaded mode in order to simplify its operation. If you are managing
 * your own threads and do not use this function you will experience crashes
 * due to race conditions.
 *
 * When using <mosquitto_loop_start>, this is set automatically.
 *
 * Parameters:
 *  mosq -     a valid mosquitto instance.
 *  threaded - true if your application is using threads, false otherwise.
 */
libmosq_EXPORT int mosquitto_threaded_set(struct mosquitto *mosq, bool threaded);

#ifdef __cplusplus
}
#endif

#endif
