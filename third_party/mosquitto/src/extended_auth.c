/*
Copyright (c) 2019-2024 Roger Light <roger@atchoo.org>

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

#include "mosquitto/mqtt_protocol.h"
#include "packet_mosq.h"
#include "property_mosq.h"
#include "util_mosq.h"


int mosquitto_ext_auth_continue(struct mosquitto *context, const char *auth_method, uint16_t auth_data_len, const void *auth_data, const mosquitto_property *input_props)
{
	struct mosquitto__packet *packet = NULL;
	int rc;
	uint32_t remaining_length;
	mosquitto_property *properties = NULL;

	rc = mosquitto_property_copy_all(&properties, input_props);
	if(rc){
		return rc;
	}

	if(!context || context->protocol != mosq_p_mqtt5 || !auth_method){
		return MOSQ_ERR_PROTOCOL;
	}

	remaining_length = 1;

	rc = mosquitto_property_add_string(&properties, MQTT_PROP_AUTHENTICATION_METHOD, auth_method);
	if(rc){
		goto error;
	}

	if(auth_data != NULL && auth_data_len > 0){
		rc = mosquitto_property_add_binary(&properties, MQTT_PROP_AUTHENTICATION_DATA, auth_data, auth_data_len);
		if(rc){
			goto error;
		}
	}

	remaining_length += mosquitto_property_get_remaining_length(properties);

	rc = packet__check_oversize(context, remaining_length);
	if(rc){
		goto error;
	}

	rc = packet__alloc(&packet, CMD_AUTH, remaining_length);
	if(rc){
		goto error;
	}

	packet__write_byte(packet, MQTT_RC_CONTINUE_AUTHENTICATION);
	property__write_all(packet, properties, true);
	mosquitto_property_free_all(&properties);

	return packet__queue(context, packet);
error:
	mosquitto_property_free_all(&properties);
	return rc;
}
