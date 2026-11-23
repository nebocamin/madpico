// SPDX-License-Identifier: Apache-2.0
//
// UDP interface for microReticulum on the Pico 2 W (arduino-pico WiFi).
//
// Compatible with the UDPInterface of Python RNS: every Reticulum packet is
// exactly one UDP datagram without additional framing. Packets are sent to
// the subnet broadcast address and received on all addresses.
//
// Note: WiFi broadcasts are neither acknowledged nor retransmitted on the
// radio layer, so occasional packet loss is expected. Prefer TCP.
//
// Matching rnsd configuration:
//   [[UDP Interface]]
//     type = UDPInterface
//     enabled = yes
//     listen_ip = 0.0.0.0
//     listen_port = 4242
//     forward_ip = <subnet>.255
//     forward_port = 4242
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <microReticulum/Interface.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Log.h>

#include <string>

class PicoUDPInterface : public RNS::InterfaceImpl {

public:
	static const uint32_t BITRATE_GUESS = 10UL * 1000 * 1000;

	PicoUDPInterface(uint16_t port = 4242, const char* name = "UDPInterface")
		: RNS::InterfaceImpl(name), _port(port) {
		_IN = true;
		_OUT = true;
		_bitrate = BITRATE_GUESS;
		_HW_MTU = 1064;
	}
	virtual ~PicoUDPInterface() { stop(); }

	// Requires WiFi to be connected already
	bool start() override {
		_online = false;
		if (WiFi.status() != WL_CONNECTED) return false;
		update_broadcast();
		if (!_udp.begin(_port)) {
			ERRORF("PicoUDPInterface: udp.begin(%u) failed", _port);
			return false;
		}
		_started = true;
		_online = true;
		INFOF("PicoUDPInterface: listening on port %u, sending to %s:%u",
			_port, _broadcast.toString().c_str(), _port);
		return true;
	}

	void stop() override {
		if (_started) _udp.stop();
		_started = false;
		_online = false;
	}

	void loop() override {
		// Handle WiFi loss/recovery
		const bool connected = (WiFi.status() == WL_CONNECTED);
		if (_online && !connected) {
			WARNING("PicoUDPInterface: WiFi connection lost");
			stop();
		}
		else if (!_online && connected) {
			INFO("PicoUDPInterface: WiFi is back, restarting");
			start();
		}
		if (!_online) return;

		// Fetch pending datagrams (bounded, so loop() is not blocked)
		for (int i = 0; i < 8; ++i) {
			int size = _udp.parsePacket();
			if (size <= 0) break;
			if (size > _HW_MTU) {
				// Drop oversized datagram
				_udp.flush();
				continue;
			}
			int len = _udp.read(_buffer.writable(size), size);
			if (len <= 0) continue;
			_buffer.resize((size_t)len);
			InterfaceImpl::handle_incoming(_buffer);
		}
	}

	std::string toString() const override {
		return "PicoUDPInterface[" + _name + ":" + std::to_string(_port) + "]";
	}

protected:
	bool send_outgoing(const RNS::Bytes& data) override {
		bool ok = false;
		if (_online) {
			ok = _udp.beginPacket(_broadcast, _port)
				&& _udp.write(data.data(), data.size()) == data.size()
				&& _udp.endPacket();
			if (!ok) WARNINGF("PicoUDPInterface: sending %u bytes failed", (unsigned)data.size());
		}
		InterfaceImpl::handle_outgoing(data);
		return ok;
	}

private:
	void update_broadcast() {
		IPAddress ip = WiFi.localIP();
		IPAddress mask = WiFi.subnetMask();
		_broadcast = IPAddress(
			ip[0] | (uint8_t)~mask[0],
			ip[1] | (uint8_t)~mask[1],
			ip[2] | (uint8_t)~mask[2],
			ip[3] | (uint8_t)~mask[3]);
	}

	WiFiUDP _udp;
	uint16_t _port;
	IPAddress _broadcast{255, 255, 255, 255};
	RNS::Bytes _buffer;
	bool _started = false;
};
