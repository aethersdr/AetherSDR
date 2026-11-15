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

#ifndef MOSQUITTO_LIBMOSQUITTO_WILL_H
#define MOSQUITTO_LIBMOSQUITTO_WILL_H

/*
 * File: mosquitto/libmosquitto_will.h
 *
 * This header contains functions for manipulating client Wills in libmosquitto.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 *
 * Section: Will
 *
 * ====================================================================== */
/*
 * Function: mosquitto_will_set
 *
 * Configure will information for a mosquitto instance. By default, clients do
 * not have a will.  This must be called before calling <mosquitto_connect>.
 *
 * It is valid to use this function for clients using all MQTT protocol versions.
 * If you need to set MQTT v5 Will properties, use <mosquitto_will_set_v5> instead.
 *
 * Parameters:
 * 	mosq -       a valid mosquitto instance.
 * 	topic -      the topic on which to publish the will.
 * 	payloadlen - the size of the payload (bytes). Valid values are between 0 and
 *               268,435,455.
 * 	payload -    pointer to the data to send. If payloadlen > 0 this must be a
 *               valid memory location.
 * 	qos -        integer value 0, 1 or 2 indicating the Quality of Service to be
 *               used for the will.
 * 	retain -     set to true to make the will a retained message.
 *
 * Returns:
 * 	MOSQ_ERR_SUCCESS -      on success.
 * 	MOSQ_ERR_INVAL -          if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -          if an out of memory condition occurred.
 * 	MOSQ_ERR_PAYLOAD_SIZE -   if payloadlen is too large.
 * 	MOSQ_ERR_MALFORMED_UTF8 - if the topic is not valid UTF-8.
 */
libmosq_EXPORT int mosquitto_will_set(struct mosquitto *mosq, const char *topic, int payloadlen, const void *payload, int qos, bool retain);

/*
 * Function: mosquitto_will_set_v5
 *
 * Configure will information for a mosquitto instance, with attached
 * properties. By default, clients do not have a will.  This must be called
 * before calling <mosquitto_connect>.
 *
 * If the mosquitto instance `mosq` is using MQTT v5, the `properties` argument
 * will be applied to the Will. For MQTT v3.1.1 and below, the `properties`
 * argument will be ignored.
 *
 * Set your client to use MQTT v5 immediately after it is created:
 *
 * mosquitto_int_option(mosq, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V5);
 *
 * Parameters:
 * 	mosq -       a valid mosquitto instance.
 * 	topic -      the topic on which to publish the will.
 * 	payloadlen - the size of the payload (bytes). Valid values are between 0 and
 *               268,435,455.
 * 	payload -    pointer to the data to send. If payloadlen > 0 this must be a
 *               valid memory location.
 * 	qos -        integer value 0, 1 or 2 indicating the Quality of Service to be
 *               used for the will.
 * 	retain -     set to true to make the will a retained message.
 * 	properties - list of MQTT 5 properties. Can be NULL. On success only, the
 * 	             property list becomes the property of libmosquitto once this
 * 	             function is called and will be freed by the library. The
 * 	             property list must be freed by the application on error.
 *
 * Returns:
 * 	MOSQ_ERR_SUCCESS -      on success.
 * 	MOSQ_ERR_INVAL -          if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -          if an out of memory condition occurred.
 * 	MOSQ_ERR_PAYLOAD_SIZE -   if payloadlen is too large.
 * 	MOSQ_ERR_MALFORMED_UTF8 - if the topic is not valid UTF-8.
 * 	MOSQ_ERR_NOT_SUPPORTED -  if properties is not NULL and the client is not
 * 	                          using MQTT v5
 * 	MOSQ_ERR_PROTOCOL -       if a property is invalid for use with wills.
 *	MOSQ_ERR_DUPLICATE_PROPERTY - if a property is duplicated where it is forbidden.
 */
libmosq_EXPORT int mosquitto_will_set_v5(struct mosquitto *mosq, const char *topic, int payloadlen, const void *payload, int qos, bool retain, mosquitto_property *properties);

/*
 * Function: mosquitto_will_clear
 *
 * Remove a previously configured will. This must be called before calling
 * <mosquitto_connect>.
 *
 * Parameters:
 * 	mosq - a valid mosquitto instance.
 *
 * Returns:
 * 	MOSQ_ERR_SUCCESS - on success.
 * 	MOSQ_ERR_INVAL -   if the input parameters were invalid.
 */
libmosq_EXPORT int mosquitto_will_clear(struct mosquitto *mosq);

#ifdef __cplusplus
}
#endif

#endif
