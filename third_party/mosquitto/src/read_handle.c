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
#include <stdio.h>
#include <string.h>

#include "mosquitto.h"
#include "logging_mosq.h"
#include "messages_mosq.h"
#include "mosquitto/mqtt_protocol.h"
#include "net_mosq.h"
#include "packet_mosq.h"
#include "read_handle.h"
#include "send_mosq.h"
#include "util_mosq.h"


int handle__packet(struct mosquitto *mosq)
{
	int rc = MOSQ_ERR_INVAL;
	assert(mosq);

	switch((mosq->in_packet.command)&0xF0){
		case CMD_PINGREQ:
			rc = handle__pingreq(mosq);
			break;
		case CMD_PINGRESP:
			rc = handle__pingresp(mosq);
			break;
		case CMD_PUBACK:
			rc = handle__pubackcomp(mosq, "PUBACK");
			break;
		case CMD_PUBCOMP:
			rc = handle__pubackcomp(mosq, "PUBCOMP");
			break;
		case CMD_PUBLISH:
			rc = handle__publish(mosq);
			break;
		case CMD_PUBREC:
			rc = handle__pubrec(mosq);
			break;
		case CMD_PUBREL:
			rc = handle__pubrel(mosq);
			break;
		case CMD_CONNACK:
			rc = handle__connack(mosq);
			break;
		case CMD_SUBACK:
			rc = handle__suback(mosq);
			break;
		case CMD_UNSUBACK:
			rc = handle__unsuback(mosq);
			break;
		case CMD_DISCONNECT:
			rc = handle__disconnect(mosq);
			break;
		case CMD_AUTH:
			rc = handle__auth(mosq);
			break;
		default:
			/* If we don't recognise the command, return an error straight away. */
			log__printf(mosq, MOSQ_LOG_ERR, "Error: Unrecognised command %d\n", (mosq->in_packet.command)&0xF0);
			rc = MOSQ_ERR_PROTOCOL;
			break;
	}

	if(mosq->protocol == mosq_p_mqtt5){
		if(rc == MOSQ_ERR_PROTOCOL || rc == MOSQ_ERR_DUPLICATE_PROPERTY){
			send__disconnect(mosq, MQTT_RC_PROTOCOL_ERROR, NULL);
		}else if(rc == MOSQ_ERR_MALFORMED_PACKET || rc == MOSQ_ERR_MALFORMED_UTF8){
			send__disconnect(mosq, MQTT_RC_MALFORMED_PACKET, NULL);
		}else if(rc == MOSQ_ERR_QOS_NOT_SUPPORTED){
			send__disconnect(mosq, MQTT_RC_QOS_NOT_SUPPORTED, NULL);
		}else if(rc == MOSQ_ERR_RETAIN_NOT_SUPPORTED){
			send__disconnect(mosq, MQTT_RC_RETAIN_NOT_SUPPORTED, NULL);
		}else if(rc == MOSQ_ERR_TOPIC_ALIAS_INVALID){
			send__disconnect(mosq, MQTT_RC_TOPIC_ALIAS_INVALID, NULL);
		}else if(rc == MOSQ_ERR_RECEIVE_MAXIMUM_EXCEEDED){
			send__disconnect(mosq, MQTT_RC_RECEIVE_MAXIMUM_EXCEEDED, NULL);
		}else if(rc == MOSQ_ERR_UNKNOWN || rc == MOSQ_ERR_NOMEM){
			send__disconnect(mosq, MQTT_RC_UNSPECIFIED, NULL);
		}
	}
	return rc;

}
