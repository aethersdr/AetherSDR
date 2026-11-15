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

/*
 * File: mosquitto/broker_control.h
 *
 * This header contains functions for use by plugins using the CONTROL event.
 */
#ifndef MOSQUITTO_BROKER_CONTROL_H
#define MOSQUITTO_BROKER_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

/* cJSON dependency removed — AetherSDR only uses the client library */
struct cJSON;
typedef struct cJSON cJSON;

#include <mosquitto/broker.h>
#include <mosquitto.h>
#include <mosquitto/mqtt_protocol.h>


/* =========================================================================
 *
 * Section: $CONTROL event helpers
 *
 * ========================================================================= */

struct mosquitto_control_cmd {
	struct mosquitto *client;
	cJSON *j_responses;
	cJSON *j_command;
	char *correlation_data;
	const char *command_name;
};

mosq_EXPORT void mosquitto_control_command_reply(struct mosquitto_control_cmd *cmd, const char *error);
mosq_EXPORT void mosquitto_control_send_response(cJSON *tree, const char *topic);
mosq_EXPORT int mosquitto_control_generic_callback(struct mosquitto_evt_control *event_data, const char *response_topic, void *userdata,
		int (*cmd_cb)(struct mosquitto_control_cmd *cmd, void *userdata));

#ifdef __cplusplus
}
#endif
#endif
