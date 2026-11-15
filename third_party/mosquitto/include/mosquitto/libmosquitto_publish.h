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

#ifndef MOSQUITTO_LIBMOSQUITTO_PUBLISH_H
#define MOSQUITTO_LIBMOSQUITTO_PUBLISH_H

/*
 * File: mosquitto/libmosquitto_publish.h
 *
 * This header contains functions for publishing with libmosquitto.
 */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Function: mosquitto_publish
 *
 * Publish a message on a given topic.
 *
 * It is valid to use this function for clients using all MQTT protocol versions.
 * If you need to set MQTT v5 PUBLISH properties, use <mosquitto_publish_v5>
 * instead.
 *
 * Parameters:
 * 	mosq -       a valid mosquitto instance.
 * 	mid -        pointer to an int. If not NULL, the function will set this
 *               to the message id of this particular message. This can be then
 *               used with the publish callback to determine when the message
 *               has been sent.
 *               Note that although the MQTT protocol doesn't use message ids
 *               for messages with QoS=0, libmosquitto assigns them message ids
 *               so they can be tracked with this parameter.
 *  topic -      null terminated string of the topic to publish to.
 * 	payloadlen - the size of the payload (bytes). Valid values are between 0 and
 *               268,435,455.
 * 	payload -    pointer to the data to send. If payloadlen > 0 this must be a
 *               valid memory location.
 * 	qos -        integer value 0, 1 or 2 indicating the Quality of Service to be
 *               used for the message.
 * 	retain -     set to true to make the message retained.
 *
 * Returns:
 * 	MOSQ_ERR_SUCCESS -        on success.
 * 	MOSQ_ERR_INVAL -          if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -          if an out of memory condition occurred.
 * 	MOSQ_ERR_NO_CONN -        if the client isn't connected to a broker.
 *	MOSQ_ERR_PROTOCOL -       if there is a protocol error communicating with the
 *                            broker.
 * 	MOSQ_ERR_PAYLOAD_SIZE -   if payloadlen is too large.
 * 	MOSQ_ERR_MALFORMED_UTF8 - if the topic is not valid UTF-8
 *	MOSQ_ERR_QOS_NOT_SUPPORTED - if the QoS is greater than that supported by
 *	                             the broker.
 *	MOSQ_ERR_OVERSIZE_PACKET - if the resulting packet would be larger than
 *	                           supported by the broker.
 *
 * See Also:
 *	<mosquitto_max_inflight_messages_set>
 */
libmosq_EXPORT int mosquitto_publish(struct mosquitto *mosq, int *mid, const char *topic, int payloadlen, const void *payload, int qos, bool retain);


/*
 * Function: mosquitto_publish_v5
 *
 * Publish a message on a given topic, with attached MQTT properties.
 *
 * Use e.g. <mosquitto_property_add_string> and similar to create a list of
 * properties, then attach them to this publish. Properties need freeing with
 * <mosquitto_property_free_all>.
 *
 * If the mosquitto instance `mosq` is using MQTT v5, the `properties` argument
 * will be applied to the PUBLISH message. For MQTT v3.1.1 and below, the
 * `properties` argument will be ignored.
 *
 * Set your client to use MQTT v5 immediately after it is created:
 *
 * mosquitto_int_option(mosq, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V5);
 *
 * Parameters:
 * 	mosq -       a valid mosquitto instance.
 * 	mid -        pointer to an int. If not NULL, the function will set this
 *               to the message id of this particular message. This can be then
 *               used with the publish callback to determine when the message
 *               has been sent.
 *               Note that although the MQTT protocol doesn't use message ids
 *               for messages with QoS=0, libmosquitto assigns them message ids
 *               so they can be tracked with this parameter.
 *  topic -      null terminated string of the topic to publish to.
 * 	payloadlen - the size of the payload (bytes). Valid values are between 0 and
 *               268,435,455.
 * 	payload -    pointer to the data to send. If payloadlen > 0 this must be a
 *               valid memory location.
 * 	qos -        integer value 0, 1 or 2 indicating the Quality of Service to be
 *               used for the message.
 * 	retain -     set to true to make the message retained.
 * 	properties - a valid mosquitto_property list, or NULL.
 *
 * Returns:
 * 	MOSQ_ERR_SUCCESS -        on success.
 * 	MOSQ_ERR_INVAL -          if the input parameters were invalid.
 * 	MOSQ_ERR_NOMEM -          if an out of memory condition occurred.
 * 	MOSQ_ERR_NO_CONN -        if the client isn't connected to a broker.
 *	MOSQ_ERR_PROTOCOL -       if there is a protocol error communicating with the
 *                            broker.
 * 	MOSQ_ERR_PAYLOAD_SIZE -   if payloadlen is too large.
 * 	MOSQ_ERR_MALFORMED_UTF8 - if the topic is not valid UTF-8
 *	MOSQ_ERR_DUPLICATE_PROPERTY - if a property is duplicated where it is forbidden.
 *	MOSQ_ERR_PROTOCOL - if any property is invalid for use with PUBLISH.
 *	MOSQ_ERR_QOS_NOT_SUPPORTED - if the QoS is greater than that supported by
 *	                             the broker.
 *	MOSQ_ERR_OVERSIZE_PACKET - if the resulting packet would be larger than
 *	                           supported by the broker.
 */
libmosq_EXPORT int mosquitto_publish_v5(
		struct mosquitto *mosq,
		int *mid,
		const char *topic,
		int payloadlen,
		const void *payload,
		int qos,
		bool retain,
		const mosquitto_property *properties);

#ifdef __cplusplus
}
#endif

#endif
