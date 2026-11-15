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
#ifndef PROPERTY_MOSQ_H
#define PROPERTY_MOSQ_H

#include "mosquitto_internal.h"

int property__read_all(int command, struct mosquitto__packet_in *packet, mosquitto_property **property);
int property__write_all(struct mosquitto__packet *packet, const mosquitto_property *property, bool write_len);

#endif
