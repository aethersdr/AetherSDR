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
#ifndef MOSQUITTO_H
#define MOSQUITTO_H

#ifndef _MSC_VER
#  define MOSQ_USED __attribute__((used))
#else
#  define MOSQ_USED
#endif

#include <mosquitto/mqtt_protocol.h>

#include <mosquitto/libmosquitto.h>
#include <mosquitto/libcommon.h>

#include <mosquitto/broker.h>
#include <mosquitto/broker_control.h>
#include <mosquitto/broker_plugin.h>

#endif
