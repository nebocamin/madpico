// SPDX-License-Identifier: Apache-2.0
//
// Network abstraction: the node is either on WiFi or on USB networking
// (the Pico appears as a CDC-NCM network adapter and uses the host's
// connection). Implemented in main.cpp.
#pragma once

#include <Arduino.h>

namespace madpico {

// True if the uplink (WiFi or USB network) has an IP address
bool net_up();

} // namespace madpico
