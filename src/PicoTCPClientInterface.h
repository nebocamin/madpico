// SPDX-License-Identifier: Apache-2.0
//
// TCP client interface for microReticulum on the Pico 2 W (arduino-pico WiFi).
//
// Wire compatible with TCPClientInterface / TCPServerInterface of Python RNS:
// HDLC-like framing (FLAG 0x7E, ESC 0x7D, ESC_MASK 0x20), one Reticulum packet
// per frame. Connects as a client to a TCPServerInterface (e.g. rnsd) and
// reconnects automatically if the connection drops.
//
// Matching rnsd configuration:
//   [[TCP Server Interface]]
//     type = TCPServerInterface
//     enabled = yes
//     listen_ip = 0.0.0.0
//     listen_port = 4242
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <microReticulum/Interface.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Log.h>
#include <microReticulum/Type.h>

#include <string>
#include <vector>

class PicoTCPClientInterface : public RNS::InterfaceImpl {

public:
	static constexpr uint32_t BITRATE_GUESS     = 10UL * 1000 * 1000;
	static constexpr uint32_t RECONNECT_WAIT_MS = 5000;   // same as Python RNS
	static constexpr uint32_t CONNECT_TIMEOUT_MS = 3000;
	// Same as Python RNS TCPClientInterface (TCP_PROBE_AFTER/INTERVAL/PROBES)
	static constexpr uint16_t KEEPALIVE_IDLE_S  = 5;
	static constexpr uint16_t KEEPALIVE_INTV_S  = 2;
	static constexpr uint8_t  KEEPALIVE_COUNT   = 12;
	// Largest accepted frame. Over TCP, Python requests a link MTU of 262144;
	// microReticulum stores the MTU as uint16_t, which truncates it to 0 and
	// falls back to 500. The 8 KB buffer leaves headroom for peers that
	// negotiate a medium link MTU.
	static constexpr uint16_t FRAME_MAX         = 8192;

	static constexpr uint8_t FLAG     = 0x7E;
	static constexpr uint8_t ESC      = 0x7D;
	static constexpr uint8_t ESC_MASK = 0x20;

	PicoTCPClientInterface(const char* host, uint16_t port, const char* name = "TCPClientInterface")
		: RNS::InterfaceImpl(name), _host(host), _port(port) {
		_IN = true;
		_OUT = true;
		_bitrate = BITRATE_GUESS;
		_HW_MTU = FRAME_MAX;
		_frame.reserve(1024);
	}
	virtual ~PicoTCPClientInterface() { stop(); }

	bool start() override {
		_online = false;
		_last_attempt_ms = millis() - RECONNECT_WAIT_MS;  // try immediately
		try_connect();
		return true;
	}

	void stop() override {
		_client.stop();
		_online = false;
	}

	void loop() override {
		if (!_client.connected()) {
			if (_online) {
				WARNINGF("TCP: connection to %s:%u lost", _host.c_str(), _port);
				_online = false;
				_client.stop();
			}
			if (millis() - _last_attempt_ms >= RECONNECT_WAIT_MS) try_connect();
			return;
		}

		// Read incoming data (bounded per iteration)
		uint8_t buf[256];
		size_t budget = 4096;
		while (budget > 0) {
			int avail = _client.available();
			if (avail <= 0) break;
			size_t want = (size_t)avail < sizeof(buf) ? (size_t)avail : sizeof(buf);
			int n = _client.read(buf, want);
			if (n <= 0) break;
			budget -= ((size_t)n < budget) ? (size_t)n : budget;
			for (int i = 0; i < n; ++i) feed(buf[i]);
		}
	}

	// Incremented on every successful (re)connect
	uint32_t connect_count() const { return _connect_count; }
	bool connected() { return _online && _client.connected(); }
	const std::string& host() const { return _host; }
	uint16_t port() const { return _port; }

	std::string toString() const override {
		return "PicoTCPClientInterface[" + _name + "/" + _host + ":" + std::to_string(_port) + "]";
	}

protected:
	bool send_outgoing(const RNS::Bytes& data) override {
		bool ok = false;
		if (_online && _client.connected()) {
			// Frame: FLAG + escape(data) + FLAG
			_tx.clear();
			_tx.reserve(data.size() + 16);
			_tx.push_back(FLAG);
			for (size_t i = 0; i < data.size(); ++i) {
				uint8_t b = data.data()[i];
				if (b == ESC || b == FLAG) {
					_tx.push_back(ESC);
					_tx.push_back(b ^ ESC_MASK);
				}
				else {
					_tx.push_back(b);
				}
			}
			_tx.push_back(FLAG);
			size_t written = _client.write(_tx.data(), _tx.size());
			ok = (written == _tx.size());
			if (!ok) {
				WARNINGF("TCP: only %u of %u bytes sent, disconnecting",
					(unsigned)written, (unsigned)_tx.size());
				_client.stop();
				_online = false;
			}
		}
		InterfaceImpl::handle_outgoing(data);
		return ok;
	}

private:
	void try_connect() {
		_last_attempt_ms = millis();
		if (WiFi.status() != WL_CONNECTED) return;
		_client.stop();
		_client.setTimeout(CONNECT_TIMEOUT_MS);
		if (!_client.connect(_host.c_str(), _port)) {
			VERBOSEF("TCP: connecting to %s:%u failed, retrying in %us",
				_host.c_str(), _port, (unsigned)(RECONNECT_WAIT_MS / 1000));
			return;
		}
		_client.setNoDelay(true);
		_client.keepAlive(KEEPALIVE_IDLE_S, KEEPALIVE_INTV_S, KEEPALIVE_COUNT);
		_in_frame = false;
		_escape = false;
		_frame.clear();
		_online = true;
		++_connect_count;
		NOTICEF("TCP: connected to %s:%u", _host.c_str(), _port);
	}

	// HDLC deframing as a state machine
	void feed(uint8_t b) {
		if (b == FLAG) {
			if (_in_frame && _frame.size() > RNS::Type::Reticulum::HEADER_MINSIZE) {
				_rx = RNS::Bytes(_frame.data(), _frame.size());
				InterfaceImpl::handle_incoming(_rx);
			}
			// Every FLAG ends a frame and starts the next one
			_in_frame = true;
			_escape = false;
			_frame.clear();
			return;
		}
		if (!_in_frame) return;
		if (b == ESC) {
			_escape = true;
			return;
		}
		if (_escape) {
			b ^= ESC_MASK;
			_escape = false;
		}
		if (_frame.size() < FRAME_MAX) {
			_frame.push_back(b);
		}
		else {
			// Oversized frame: drop until the next FLAG
			_in_frame = false;
			_frame.clear();
		}
	}

	WiFiClient _client;
	std::string _host;
	uint16_t _port;
	uint32_t _last_attempt_ms = 0;
	uint32_t _connect_count = 0;

	bool _in_frame = false;
	bool _escape = false;
	std::vector<uint8_t> _frame;
	std::vector<uint8_t> _tx;
	RNS::Bytes _rx;
};
