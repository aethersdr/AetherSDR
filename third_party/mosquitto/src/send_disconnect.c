/*
Copyright (c) 2009-2021 Roger Light <roger@atchoo.org>

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

#include <assert.h>

#ifdef WITH_BROKER
#  include "mosquitto_broker_internal.h"
#  include "sys_tree.h"
#endif

#include "mosquitto.h"
#include "mosquitto_internal.h"
#include "logging_mosq.h"
#include "mosquitto/mqtt_protocol.h"
#include "packet_mosq.h"
#include "property_mosq.h"
#include "send_mosq.h"


int send__disconnect(struct mosquitto *mosq, uint8_t reason_code, const mosquitto_property *properties)
{
	struct mosquitto__packet *packet = NULL;
	int rc;
	uint32_t remaining_length = 0;

	assert(mosq);
#ifdef WITH_BROKER
#  ifdef WITH_BRIDGE
	if(mosq->bridge){
		log__printf(mosq, MOSQ_LOG_DEBUG, "Bridge %s sending DISCONNECT", SAFE_PRINT(mosq->id));
	}else
#  else
	{
		log__printf(mosq, MOSQ_LOG_DEBUG, "Sending DISCONNECT to %s (rc%d)", SAFE_PRINT(mosq->id), reason_code);
	}
#  endif
#else
	log__printf(mosq, MOSQ_LOG_DEBUG, "Client %s sending DISCONNECT", SAFE_PRINT(mosq->id));
#endif

	if(mosq->protocol == mosq_p_mqtt5 && (reason_code != 0 || properties)){
		remaining_length = 1;
		if(properties){
			remaining_length += mosquitto_property_get_remaining_length(properties);
		}
	}else{
		remaining_length = 0;
	}

	rc = packet__alloc(&packet, CMD_DISCONNECT, remaining_length);
	if(rc){
		mosquitto_FREE(packet);
		return rc;
	}
	if(remaining_length > 0){
		packet__write_byte(packet, reason_code);
		if(properties){
			property__write_all(packet, properties, true);
		}
	}

#ifdef WITH_BROKER
	metrics__int_inc(mosq_counter_mqtt_disconnect_sent, 1);
#endif
	return packet__queue(mosq, packet);
}

