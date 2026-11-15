/*
Copyright (c) 2018-2021 Roger Light <roger@atchoo.org>

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

#include <errno.h>
#include <string.h>

#ifndef WIN32
#  include <strings.h>
#endif

#include "logging_mosq.h"
#include "mosquitto/mqtt_protocol.h"
#include "packet_mosq.h"
#include "property_common.h"
#include "property_mosq.h"


static int property__read(struct mosquitto__packet_in *packet, uint32_t *len, mosquitto_property *property)
{
	int rc;
	uint32_t property_identifier;
	uint8_t byte;
	uint8_t byte_count;
	uint16_t uint16;
	uint32_t uint32;
	uint32_t varint;
	char *str1, *str2;
	uint16_t slen1, slen2;

	if(!property){
		return MOSQ_ERR_INVAL;
	}

	rc = packet__read_varint(packet, &property_identifier, NULL);
	if(rc){
		return rc;
	}
	*len -= mosquitto_varint_bytes(property_identifier);

	memset(property, 0, sizeof(mosquitto_property));

	property->identifier = (int32_t)property_identifier;

	switch(property_identifier){
		case MQTT_PROP_PAYLOAD_FORMAT_INDICATOR:
		case MQTT_PROP_REQUEST_PROBLEM_INFORMATION:
		case MQTT_PROP_REQUEST_RESPONSE_INFORMATION:
		case MQTT_PROP_MAXIMUM_QOS:
		case MQTT_PROP_RETAIN_AVAILABLE:
		case MQTT_PROP_WILDCARD_SUB_AVAILABLE:
		case MQTT_PROP_SUBSCRIPTION_ID_AVAILABLE:
		case MQTT_PROP_SHARED_SUB_AVAILABLE:
			rc = packet__read_byte(packet, &byte);
			if(rc){
				return rc;
			}
			*len -= 1; /* byte */
			property->value.i8 = byte;
			property->property_type = MQTT_PROP_TYPE_BYTE;
			break;

		case MQTT_PROP_SERVER_KEEP_ALIVE:
		case MQTT_PROP_RECEIVE_MAXIMUM:
		case MQTT_PROP_TOPIC_ALIAS_MAXIMUM:
		case MQTT_PROP_TOPIC_ALIAS:
			rc = packet__read_uint16(packet, &uint16);
			if(rc){
				return rc;
			}
			*len -= 2; /* uint16 */
			property->value.i16 = uint16;
			property->property_type = MQTT_PROP_TYPE_INT16;
			break;

		case MQTT_PROP_MESSAGE_EXPIRY_INTERVAL:
		case MQTT_PROP_SESSION_EXPIRY_INTERVAL:
		case MQTT_PROP_WILL_DELAY_INTERVAL:
		case MQTT_PROP_MAXIMUM_PACKET_SIZE:
			rc = packet__read_uint32(packet, &uint32);
			if(rc){
				return rc;
			}
			*len -= 4; /* uint32 */
			property->value.i32 = uint32;
			property->property_type = MQTT_PROP_TYPE_INT32;
			break;

		case MQTT_PROP_SUBSCRIPTION_IDENTIFIER:
			rc = packet__read_varint(packet, &varint, &byte_count);
			if(rc){
				return rc;
			}
			*len -= byte_count;
			property->value.varint = varint;
			property->property_type = MQTT_PROP_TYPE_VARINT;
			break;

		case MQTT_PROP_CONTENT_TYPE:
		case MQTT_PROP_RESPONSE_TOPIC:
		case MQTT_PROP_ASSIGNED_CLIENT_IDENTIFIER:
		case MQTT_PROP_AUTHENTICATION_METHOD:
		case MQTT_PROP_RESPONSE_INFORMATION:
		case MQTT_PROP_SERVER_REFERENCE:
		case MQTT_PROP_REASON_STRING:
			rc = packet__read_string(packet, &str1, &slen1);
			if(rc){
				return rc;
			}
			*len = (*len) - 2 - slen1; /* uint16, string len */
			property->value.s.v = str1;
			property->value.s.len = slen1;
			property->property_type = MQTT_PROP_TYPE_STRING;
			break;

		case MQTT_PROP_AUTHENTICATION_DATA:
		case MQTT_PROP_CORRELATION_DATA:
			rc = packet__read_binary(packet, (uint8_t **)&str1, &slen1);
			if(rc){
				return rc;
			}
			*len = (*len) - 2 - slen1; /* uint16, binary len */
			property->value.bin.v = str1;
			property->value.bin.len = slen1;
			property->property_type = MQTT_PROP_TYPE_BINARY;
			break;

		case MQTT_PROP_USER_PROPERTY:
			rc = packet__read_string(packet, &str1, &slen1);
			if(rc){
				return rc;
			}
			*len = (*len) - 2 - slen1; /* uint16, string len */

			rc = packet__read_string(packet, &str2, &slen2);
			if(rc){
				mosquitto_FREE(str1);
				return rc;
			}
			*len = (*len) - 2 - slen2; /* uint16, string len */

			property->name.v = str1;
			property->name.len = slen1;
			property->value.s.v = str2;
			property->value.s.len = slen2;
			property->property_type = MQTT_PROP_TYPE_STRING_PAIR;
			break;

		default:
#ifdef WITH_BROKER
			log__printf(NULL, MOSQ_LOG_DEBUG, "Unsupported property type: %d", property_identifier);
#endif
			return MOSQ_ERR_MALFORMED_PACKET;
	}

	return MOSQ_ERR_SUCCESS;
}


int property__read_all(int command, struct mosquitto__packet_in *packet, mosquitto_property **properties)
{
	int rc;
	uint32_t proplen;
	mosquitto_property *p, *tail = NULL;

	rc = packet__read_varint(packet, &proplen, NULL);
	if(rc){
		return rc;
	}

	*properties = NULL;

	/* The order of properties must be preserved for some types, so keep the
	 * same order for all */
	while(proplen > 0){
		p = mosquitto_calloc(1, sizeof(mosquitto_property));
		if(!p){
			mosquitto_property_free_all(properties);
			return MOSQ_ERR_NOMEM;
		}

		rc = property__read(packet, &proplen, p);
		if(rc){
			mosquitto_FREE(p);
			mosquitto_property_free_all(properties);
			return rc;
		}

		if(!(*properties)){
			*properties = p;
		}else{
			tail->next = p;
		}
		tail = p;

	}

	rc = mosquitto_property_check_all(command, *properties);
	if(rc){
		mosquitto_property_free_all(properties);
		return rc;
	}
	return MOSQ_ERR_SUCCESS;
}


static int property__write(struct mosquitto__packet *packet, const mosquitto_property *property)
{
	int rc;

	rc = packet__write_varint(packet, (uint32_t)mosquitto_property_identifier(property));
	if(rc){
		return rc;
	}

	switch(property->property_type){
		case MQTT_PROP_TYPE_BYTE:
			packet__write_byte(packet, property->value.i8);
			break;

		case MQTT_PROP_TYPE_INT16:
			packet__write_uint16(packet, property->value.i16);
			break;

		case MQTT_PROP_TYPE_INT32:
			packet__write_uint32(packet, property->value.i32);
			break;

		case MQTT_PROP_TYPE_VARINT:
			return packet__write_varint(packet, property->value.varint);

		case MQTT_PROP_TYPE_STRING:
			packet__write_string(packet, property->value.s.v, property->value.s.len);
			break;

		case MQTT_PROP_TYPE_BINARY:
			packet__write_uint16(packet, property->value.bin.len);
			packet__write_bytes(packet, property->value.bin.v, property->value.bin.len);
			break;

		case MQTT_PROP_TYPE_STRING_PAIR:
			packet__write_string(packet, property->name.v, property->name.len);
			packet__write_string(packet, property->value.s.v, property->value.s.len);
			break;

		default:
#ifdef WITH_BROKER
			log__printf(NULL, MOSQ_LOG_DEBUG, "Unsupported property type: %d", property->identifier);
#endif
			return MOSQ_ERR_INVAL;
	}

	return MOSQ_ERR_SUCCESS;
}


int property__write_all(struct mosquitto__packet *packet, const mosquitto_property *properties, bool write_len)
{
	int rc;
	const mosquitto_property *p;

	if(write_len){
		rc = packet__write_varint(packet, mosquitto_property_get_length_all(properties));
		if(rc){
			return rc;
		}
	}

	p = properties;
	while(p){
		rc = property__write(packet, p);
		if(rc){
			return rc;
		}
		p = p->next;
	}

	return MOSQ_ERR_SUCCESS;
}
