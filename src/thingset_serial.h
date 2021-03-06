/* LibreSolar charge controller firmware
 * Copyright (c) 2016-2019 Martin Jäger (www.libre.solar)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef THINGSET_SERIAL_H
#define THINGSET_SERIAL_H

/** @file
 *
 * @brief
 * ThingSet protocol based communication via UART or USB serial port
 */

#include "mbed.h"

/** UART serial interface (either in UEXT connector or from additional SWD serial)
 */
void thingset_serial_init(Serial* s);
void thingset_serial_process_asap();
void thingset_serial_process_1s();


/** UART serial interface (either in UEXT connector or from additional SWD serial)
 */
void uart_serial_init(Serial* s);
void uart_serial_process();
void uart_serial_pub();

/** Serial interface via USB CDC device class (currently only supported with STM32F0)
 */
void usb_serial_init();
void usb_serial_process();
void usb_serial_pub();

#endif /* THINGSET_SERIAL_H */
