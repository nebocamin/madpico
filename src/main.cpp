// SPDX-License-Identifier: Apache-2.0
//
// madpico - NomadNet compatible page node for the Raspberry Pi Pico 2 W
//
// Built on microReticulum (C++ port of Reticulum). The request handling is
// based on the nomadnet example of microReticulum, Copyright (c) 2026 Chad
// Attermann, licensed under the Apache License 2.0.
//
// Pages (*.mu) are stored in LittleFS under /pages and served as /page/<name>,
// like NomadNet and rns-page-node do. /page/status.mu is a built-in dynamic
// page. Pages can be managed over USB serial with tools/madpico.py.

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <MsgPack.h>
#include <pico/cyw43_arch.h>

#include <microReticulum.h>

#include <algorithm>
#include <set>
#include <vector>

#include "config.h"
#include "DefaultPages.h"
#include "Net.h"
#include "Diagnostics.h"
#include "FileTransfer.h"
#include "PicoEntropy.h"
#include "PicoLittleFS.h"
#include "PicoTCPClientInterface.h"
#include "PicoUDPInterface.h"

#if USE_USB_NET
#include <NCMEthernetlwIP.h>
static NCMEthernetlwIP usbnet;
#endif

bool madpico::net_up() {
#if USE_USB_NET
	return usbnet.connected();
#else
	return WiFi.status() == WL_CONNECTED;
#endif
}

// Short description of the uplink for logs and the status page
static String uplink_info(bool show_address) {
#if USE_USB_NET
	if (!usbnet.connected()) return "USB network, no IP";
	return show_address ? "USB network, IP " + usbnet.localIP().toString() : String("USB network");
#else
	if (WiFi.status() != WL_CONNECTED) return "WiFi disconnected";
	String s = show_address ? "WiFi \"" + WiFi.SSID() + "\", IP " + WiFi.localIP().toString() + ", " : String("WiFi ");
	return s + String(WiFi.RSSI()) + " dBm";
#endif
}

// ---------------------------------------------------------------------------
// Global state
// Note: Reticulum must NOT be created globally with its default constructor.
// It logs via Serial before arduino-pico has initialised USB, which hangs the
// board before setup() is reached.
// ---------------------------------------------------------------------------
static madpico::PicoLittleFS filesystem;
static RNS::Reticulum reticulum({RNS::Type::NONE});
static RNS::Interface udp_interface({RNS::Type::NONE});
static RNS::Interface tcp_interface({RNS::Type::NONE});
static PicoTCPClientInterface* tcp_impl = nullptr;  // owned by tcp_interface
static RNS::Identity node_identity({RNS::Type::NONE});
static RNS::Destination node_destination({RNS::Type::NONE});
static std::set<std::string> registered_pages;  // request paths, e.g. "/page/index.mu"
static madpico::Diagnostics diag;

static uint32_t tcp_seen_connects = 0;
static uint32_t last_announce_ms = 0;
static uint32_t last_wifi_attempt_ms = 0;
static uint32_t last_link_check_ms = 0;
static uint32_t requests_served = 0;
static uint32_t links_total = 0;
static uint32_t links_closed_idle = 0;
static uint32_t links_closed_limit = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static RNS::Bytes msgpack_bin(const uint8_t* data, size_t len) {
	MsgPack::Packer packer;
	packer.packBinary(data, len);
	return RNS::Bytes(packer.data(), packer.size());
}

static bool wifi_connect(uint32_t timeout_ms) {
	if (WiFi.status() == WL_CONNECTED) return true;
	struct { const char* ssid; const char* pass; } networks[] = WIFI_NETWORKS;
	// Note: WiFi.mode() sets an ESP compatibility flag in arduino-pico which
	// makes WiFi.begin() return immediately, so we wait ourselves.
	WiFi.mode(WIFI_STA);
	WiFi.setHostname(NODE_NAME);
	for (auto& n : networks) {
		Serial.printf("[wifi] Connecting to \"%s\" ...\n", n.ssid);
		last_wifi_attempt_ms = millis();
		WiFi.begin(n.ssid, n.pass);
		uint32_t start = millis();
		while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
			delay(100);
			rp2040.wdt_reset();  // no-op before the watchdog is started
		}
		if (WiFi.status() == WL_CONNECTED) {
			Serial.printf("[wifi] Connected to \"%s\", IP %s, RSSI %d dBm\n",
				WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
			return true;
		}
		int link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
		const char* reason =
			link == CYW43_LINK_BADAUTH ? "wrong password / auth rejected" :
			link == CYW43_LINK_NONET   ? "network not found" :
			link == CYW43_LINK_FAIL    ? "connection failed" :
			link == CYW43_LINK_JOIN    ? "joined, but no IP (DHCP?)" : "unknown";
		Serial.printf("[wifi] \"%s\": %s (cyw43 link %d)\n", n.ssid, reason, link);
		WiFi.disconnect();
	}
	last_wifi_attempt_ms = millis();
	Serial.println("[wifi] No network reachable, retrying later");
	return false;
}

static void wifi_scan() {
	int n = WiFi.scanNetworks();
	Serial.printf("[wifi] Scan: %d networks\n", n);
	for (int i = 0; i < n; ++i) {
		uint8_t bssid[6];
		WiFi.BSSID(i, bssid);
		Serial.printf("[wifi]   \"%s\" %02X:%02X:%02X:%02X:%02X:%02X ch %d, %d dBm, enc %d\n",
			WiFi.SSID(i), bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
			WiFi.channel(i), WiFi.RSSI(i), (int)WiFi.encryptionType(i));
	}
}

static bool any_interface_online() {
	if (tcp_impl && tcp_impl->connected()) return true;
	if (udp_interface && udp_interface.online()) return true;
	return false;
}

// ---------------------------------------------------------------------------
// Clock
//
// The Pico has no RTC. microReticulum's clock is "milliseconds since boot +
// persisted offset", i.e. somewhere in 1970, and it can even jump back by up
// to 10 minutes after a reboot because the offset is only persisted every
// 10 minutes. Announces carry that time as emission timestamp, and Python RNS
// transport nodes only accept an announce for a known destination if its
// timestamp is newer than the last one. Without real time, announces after a
// quick reboot are silently dropped by the network.
// ---------------------------------------------------------------------------
static constexpr time_t MIN_VALID_EPOCH = 1700000000;  // Nov 2023
static bool clock_synced = false;
static bool ntp_started = false;

// Moves microReticulum's clock to NTP time (forward only). Returns true if
// the clock was adjusted.
static bool sync_clock() {
	if (clock_synced || !madpico::net_up()) return false;
	if (!ntp_started) {
		NTP.begin(NTP_SERVER_1, NTP_SERVER_2);
		ntp_started = true;
	}
	time_t now = time(nullptr);
	if (now < MIN_VALID_EPOCH) return false;  // not synchronised yet
	clock_synced = true;

	const uint64_t ntp_ms = (uint64_t)now * 1000ULL;
	const uint64_t rns_ms = RNS::Utilities::OS::ltime();
	if (ntp_ms <= rns_ms) {
		Serial.println("[time] Clock already ahead of NTP, not adjusting");
		return false;
	}
	RNS::Utilities::OS::setTimeOffset(RNS::Utilities::OS::getTimeOffset() + (ntp_ms - rns_ms));
	// Persist right away (same format as Reticulum::writeTimeOffset(), which
	// is protected), so the clock stays sane after a reboot without NTP
	uint64_t persisted = RNS::Utilities::OS::ltime();
	RNS::Utilities::OS::write_file("./time_offset", RNS::Bytes((uint8_t*)&persisted, sizeof(persisted)));
	Serial.printf("[time] Clock set from NTP: %lu (unix time)\n", (unsigned long)now);
	return true;
}

// ---------------------------------------------------------------------------
// Announces
// ---------------------------------------------------------------------------
// Announce after every TCP (re)connect: a Python transport node drops the
// path when the old connection goes away and only learns the new one from
// an announce. If the node is rate-limited, the announce still updates the
// routing data, it just isn't rebroadcast.
static bool announced = false;  // last_announce_ms is valid

static void announce() {
	if (!any_interface_online()) {
		// Not counted as sent, so we announce right after (re)connecting
		Serial.println("[rns] Announce postponed, no interface online");
		return;
	}
	node_destination.announce(RNS::Bytes(NODE_NAME));
	last_announce_ms = millis();
	announced = true;
	Serial.printf("[rns] Announce sent: %s \"%s\"\n",
		node_destination.hash().toHex().c_str(), NODE_NAME);
}

// ---------------------------------------------------------------------------
// Link limits
// ---------------------------------------------------------------------------

// Seconds since the last data packet on the link (keepalives don't count),
// or since activation if no data was exchanged yet
static double link_idle_s(RNS::Link& link) {
	return std::min(link.no_data_for(), link.get_age());
}

static void enforce_link_limits() {
	const auto& active = RNS::Transport::active_links();
	if (active.empty()) return;
	std::vector<RNS::Link> links(active.begin(), active.end());

	std::vector<RNS::Link> remaining;
	for (auto& link : links) {
		if (link.status() != RNS::Type::Link::ACTIVE) continue;
		if (link_idle_s(link) > LINK_IDLE_TIMEOUT_S) {
			Serial.printf("[link] Closing idle link <%s>\n", link.hash().toHex().c_str());
			link.teardown();
			++links_closed_idle;
		}
		else {
			remaining.push_back(link);
		}
	}

	if (remaining.size() > MAX_ACTIVE_LINKS) {
		// Close the links that have been idle the longest
		std::sort(remaining.begin(), remaining.end(), [](RNS::Link& a, RNS::Link& b) {
			return link_idle_s(a) > link_idle_s(b);
		});
		size_t excess = remaining.size() - MAX_ACTIVE_LINKS;
		for (size_t i = 0; i < excess; ++i) {
			Serial.printf("[link] Link limit (%u) reached, closing <%s>\n",
				(unsigned)MAX_ACTIVE_LINKS, remaining[i].hash().toHex().c_str());
			remaining[i].teardown();
			++links_closed_limit;
		}
	}
}

static void link_established(RNS::Link& link) {
	++links_total;
	Serial.printf("[link] Established <%s>\n", link.hash().toHex().c_str());
}

// ---------------------------------------------------------------------------
// Request handlers
// ---------------------------------------------------------------------------
static RNS::Bytes error_page(const char* message) {
	std::string page = ">Error\n\n" + std::string(message) + "\n";
	return msgpack_bin((const uint8_t*)page.data(), page.size());
}

// Serves a file from /pages. The registered request path "/page/foo.mu"
// maps to the file "/pages/foo.mu".
static RNS::Bytes serve_file_page(const RNS::Bytes& path, const RNS::Bytes& data,
		const RNS::Bytes& request_id, const RNS::Bytes& link_id,
		const RNS::Identity& remote_identity, double requested_at) {
	std::string req = path.toString();
	std::string file = std::string(PAGES_DIR) + req.substr(strlen("/page"));
	Serial.printf("[page] %s\n", req.c_str());

	fs::File f = LittleFS.open(file.c_str(), "r");
	if (!f) return error_page("Page not found.");
	size_t size = f.size();
	if (size > MAX_PAGE_SIZE) {
		f.close();
		return error_page("Page too large.");
	}
	RNS::Bytes content;
	size_t len = f.read(content.writable(size), size);
	content.resize(len);
	f.close();
	++requests_served;
	return msgpack_bin(content.data(), content.size());
}

// Built-in dynamic status page
static RNS::Bytes serve_status_page(const RNS::Bytes& path, const RNS::Bytes& data,
		const RNS::Bytes& request_id, const RNS::Bytes& link_id,
		const RNS::Identity& remote_identity, double requested_at) {
	Serial.printf("[page] %s (dynamic)\n", path.toString().c_str());
	++requests_served;
	uint32_t up = millis() / 1000;
	FSInfo fsinfo;
	LittleFS.info(fsinfo);
	// printf has no float support -> tenths of a degree as integer
	int temp10 = (int)(analogReadTemp() * 10.0f);

	std::string ifaces;
#if USE_TCP
	ifaces += "TCP " TCP_TARGET_HOST ":" + std::to_string(TCP_TARGET_PORT)
		+ (tcp_impl && tcp_impl->connected() ? " (connected)" : " (disconnected)");
#endif
#if USE_UDP
	if (!ifaces.empty()) ifaces += ", ";
	ifaces += "UDP broadcast :" + std::to_string(UDP_PORT);
#endif

	String uplink = uplink_info(STATUS_SHOW_NETWORK);

	char page[1024];
	snprintf(page, sizeof(page),
		">%s status\n"
		"\n"
		"`!Address`!    %s\n"
		"`!Firmware`!   madpico %s\n"
		"`!Uptime`!     %lud %02lu:%02lu:%02lu\n"
		"`!Free heap`!  %d / %d bytes\n"
		"`!Flash (FS)`! %lu / %lu bytes used\n"
		"`!Uplink`!     %s\n"
		"`!Interface`!  %s\n"
		"`!Links`!      %u active (max %u), %lu total\n"
		"`!CPU`!        RP2350 @ %lu MHz, %d.%d C\n"
		"`!Requests`!   %lu\n"
		"\n"
		"`F00a`_`[Back`:/page/index.mu]`_`f\n",
		NODE_NAME,
		node_destination.hash().toHex().c_str(),
		FW_VERSION,
		(unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
		(unsigned long)(up / 60 % 60), (unsigned long)(up % 60),
		rp2040.getFreeHeap(), rp2040.getTotalHeap(),
		(unsigned long)fsinfo.usedBytes, (unsigned long)fsinfo.totalBytes,
		uplink.c_str(),
		ifaces.c_str(),
		(unsigned)RNS::Transport::active_links().size(), (unsigned)MAX_ACTIVE_LINKS,
		(unsigned long)links_total,
		(unsigned long)(rp2040.f_cpu() / 1000000), temp10 / 10, abs(temp10 % 10),
		(unsigned long)requests_served);
	return msgpack_bin((const uint8_t*)page, strlen(page));
}

// ---------------------------------------------------------------------------
// Page registration
// ---------------------------------------------------------------------------
static void register_page(const std::string& rel_path) {
	std::string req = "/page/" + rel_path;
	if (req == STATUS_PAGE_PATH) return;  // built-in page takes precedence
	if (registered_pages.count(req)) return;
	node_destination.register_request_handler(
		RNS::Bytes(req), serve_file_page, RNS::Type::Destination::ALLOW_ALL);
	registered_pages.insert(req);
	Serial.printf("[page] Registered %s\n", req.c_str());
}

static void unregister_page(const std::string& rel_path) {
	std::string req = "/page/" + rel_path;
	if (!registered_pages.count(req)) return;
	node_destination.deregister_request_handler(RNS::Bytes(req));
	registered_pages.erase(req);
	Serial.printf("[page] Unregistered %s\n", req.c_str());
}

// Registers all files below /pages recursively (hidden files are skipped)
static void register_pages(const std::string& dir, const std::string& rel) {
	fs::Dir d = LittleFS.openDir(dir.c_str());
	while (d.next()) {
		std::string name = d.fileName().c_str();
		if (name.empty() || name[0] == '.') continue;
		std::string r = rel.empty() ? name : rel + "/" + name;
		if (d.isDirectory()) register_pages(dir + "/" + name, r);
		else register_page(r);
	}
}

static void write_default_page(const char* path, const char* content, size_t len, bool overwrite) {
	if (!overwrite && LittleFS.exists(path)) return;
	fs::File f = LittleFS.open(path, "w");
	if (f) {
		f.write((const uint8_t*)content, len);
		f.close();
		Serial.printf("[page] Wrote default page %s\n", path);
	}
}

static void ensure_default_pages(bool overwrite = false) {
	if (!LittleFS.exists(PAGES_DIR)) LittleFS.mkdir(PAGES_DIR);
	write_default_page(PAGES_DIR "/index.mu", DEFAULT_INDEX_PAGE, sizeof(DEFAULT_INDEX_PAGE) - 1, overwrite);
	write_default_page(PAGES_DIR "/about.mu", DEFAULT_ABOUT_PAGE, sizeof(DEFAULT_ABOUT_PAGE) - 1, overwrite);
}

static madpico::FileTransfer file_transfer(PAGES_DIR, MAX_PAGE_SIZE, register_page, unregister_page);

// ---------------------------------------------------------------------------
// Serial console
//   single characters:  a = announce, s = status, w = WiFi scan,
//                       c = reconnect WiFi, r = rewrite default pages + reboot
//   lines starting with '!': file transfer protocol (see FileTransfer.h)
// ---------------------------------------------------------------------------
static void print_status() {
	Serial.printf("Uptime %lus, %s, TCP %s (%lu connects), free heap %d (min %lu), "
		"links %u active / %u pending (%lu total, %lu idle-closed, %lu limit-closed), "
		"pages %u, requests %lu, clock %s (rns time %lu)\n",
		(unsigned long)(millis() / 1000),
		uplink_info(true).c_str(),
		tcp_impl ? (tcp_impl->connected() ? "connected" : "disconnected") : "off",
		(unsigned long)(tcp_impl ? tcp_impl->connect_count() : 0),
		rp2040.getFreeHeap(), (unsigned long)diag.min_heap(),
		(unsigned)RNS::Transport::active_links().size(),
		(unsigned)RNS::Transport::pending_links().size(),
		(unsigned long)links_total, (unsigned long)links_closed_idle, (unsigned long)links_closed_limit,
		(unsigned)registered_pages.size(),
		(unsigned long)requests_served,
		clock_synced ? "NTP" : "not synced",
		(unsigned long)RNS::Utilities::OS::time());
}

static void handle_serial() {
	static std::string line;
	static bool in_line = false;
	while (Serial.available()) {
		char c = Serial.read();
		if (in_line) {
			if (c == '\n') {
				in_line = false;
				file_transfer.handle_line(line);
				line.clear();
			}
			else if (c != '\r' && line.size() < 200) {
				line += c;
			}
			continue;
		}
		switch (c) {
			case '!': in_line = true; line.clear(); break;
			case 'a':
				if (!node_destination) break;
				if (announced && millis() - last_announce_ms < 60UL * 60UL * 1000UL) {
					Serial.println("[rns] Note: transport nodes rate-limit frequent announces; "
						"too many get the node's announces dropped for a while");
				}
				announce();
				break;
			case 's':
				print_status();
				// Table sizes (needs -DRNS_DEBUG_METRICS)
				RNS::loglevel(RNS::LOG_VERBOSE);
				RNS::Transport::dump_stats();
				RNS::loglevel(RNS::LOG_NOTICE);
				break;
#if !USE_USB_NET
			case 'w': wifi_scan(); break;
			case 'c': WiFi.disconnect(); wifi_connect(20000); break;
#endif
			case 'r':
				Serial.println("Rewriting default pages and rebooting ...");
				ensure_default_pages(true);
				delay(200);
				rp2040.reboot();
				break;
			default: break;
		}
	}
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
static void setup_node() {
	if (!filesystem.init()) {
		throw std::runtime_error("LittleFS initialisation failed");
	}
	RNS::Utilities::OS::register_filesystem(filesystem);
	Serial.printf("[fs] LittleFS: %u / %u bytes free\n",
		(unsigned)filesystem.storageAvailable(), (unsigned)filesystem.storageSize());

	// Reticulum (calls RNG.begin()) + hardware entropy
	reticulum = RNS::Reticulum();
	pico_seed_rng();
	RNS::loglevel(RNS::LOG_NOTICE);

#if USE_USB_NET
	// The host has to provide DHCP and NAT, e.g. NetworkManager's
	// "Shared to other computers" mode
	usbnet.setDefault(true);
	if (!usbnet.begin()) {
		Serial.println("[usb] Starting USB network failed");
	}
	else {
		Serial.println("[usb] USB network adapter started, waiting for DHCP from the host ...");
		uint32_t t0 = millis();
		while (!usbnet.connected() && millis() - t0 < 30000) delay(100);
		if (usbnet.connected()) {
			Serial.printf("[usb] Got IP %s, gateway %s\n", usbnet.localIP().toString().c_str(),
				usbnet.gatewayIP().toString().c_str());
		}
		else {
			Serial.println("[usb] No IP yet (is connection sharing enabled on the host?)");
		}
	}
#else
	// The first join right after boot often fails (freshly initialised
	// CYW43), so try several rounds at startup
	for (int round = 0; round < 3 && !wifi_connect(15000); ++round) delay(1000);
#endif

	// Get real time before the first announce (see sync_clock())
	if (madpico::net_up()) {
		NTP.begin(NTP_SERVER_1, NTP_SERVER_2);
		ntp_started = true;
		if (!NTP.waitSet(10000)) Serial.println("[time] NTP not answering yet, will keep trying");
	}

#if USE_UDP
	udp_interface = new PicoUDPInterface(UDP_PORT);
	RNS::Transport::register_interface(udp_interface);
	udp_interface.start();
#endif
#if USE_TCP
	tcp_impl = new PicoTCPClientInterface(TCP_TARGET_HOST, TCP_TARGET_PORT);
	tcp_interface = tcp_impl;
	RNS::Transport::register_interface(tcp_interface);
	tcp_interface.start();
	tcp_seen_connects = tcp_impl->connect_count();
	Serial.printf("[tcp] Target %s:%u, %s\n", TCP_TARGET_HOST, TCP_TARGET_PORT,
		tcp_impl->connected() ? "connected" : "not connected yet");
#endif

	reticulum.transport_enabled(false);
	reticulum.start();
	// microReticulum only applies these caps in transport mode. Without them
	// the in-RAM tables would grow without bound on a public network.
	RNS::Identity::known_destinations_maxsize(KNOWN_DESTINATIONS_MAX);
	RNS::Transport::path_table_maxsize(PATH_TABLE_MAX);
	// Must run after reticulum.start(), which loads the persisted time offset
	sync_clock();

	// Persistent node identity
	node_identity = RNS::Identity::from_file(IDENTITY_PATH);
	if (node_identity) {
		Serial.println("[rns] Loaded identity from flash");
	}
	else {
		node_identity = RNS::Identity();
		if (node_identity.to_file(IDENTITY_PATH)) {
			Serial.println("[rns] Created and saved new identity");
		}
		else {
			Serial.println("[rns] WARNING: could not save identity");
		}
	}

	node_destination = RNS::Destination(node_identity,
		RNS::Type::Destination::IN, RNS::Type::Destination::SINGLE,
		APP_NAME, APP_ASPECT);
	node_destination.set_link_established_callback(link_established);

	ensure_default_pages();
	register_pages(PAGES_DIR, "");
	node_destination.register_request_handler(
		RNS::Bytes(STATUS_PAGE_PATH), serve_status_page, RNS::Type::Destination::ALLOW_ALL);
	Serial.printf("[page] %u page(s) + %s\n", (unsigned)registered_pages.size(), STATUS_PAGE_PATH);

	Serial.printf("[rns] Node \"%s\" running, address %s\n",
		NODE_NAME, node_destination.hash().toHex().c_str());
	tcp_seen_connects = tcp_impl ? tcp_impl->connect_count() : 0;
	if (any_interface_online()) announce();
}

void setup() {
	pinMode(LED_BUILTIN, OUTPUT);
	digitalWrite(LED_BUILTIN, HIGH);

	Serial.begin(115200);
	uint32_t start = millis();
	while (!Serial && millis() - start < 3000) delay(10);

	Serial.println();
	Serial.printf("=== madpico %s - NomadNet page node ===\n", FW_VERSION);
	diag.report_previous_run();
	try {
		setup_node();
	}
	catch (const std::exception& e) {
		Serial.printf("ERROR during startup: %s\n", e.what());
	}
	Serial.println("Serial commands: a = announce, s = status, w = WiFi scan, c = reconnect WiFi,");
	Serial.println("                 r = rewrite default pages + reboot");

	// Reboot automatically if the main loop hangs. The diagnostics report
	// the reset reason and the last loop stage on the next boot.
	rp2040.wdt_begin(WATCHDOG_TIMEOUT_MS);
}

static void loop_work() {
	diag.stage(madpico::STAGE_RETICULUM);
	reticulum.loop();

	// WiFi reconnect (the interfaces restart themselves afterwards)
	diag.stage(madpico::STAGE_WIFI);
#if !USE_USB_NET
	if (WiFi.status() != WL_CONNECTED && millis() - last_wifi_attempt_ms > WIFI_RETRY_MS) {
		wifi_connect(10000);
	}
#endif

	diag.stage(madpico::STAGE_ANNOUNCE);
	if (node_destination && any_interface_online()) {
		// Periodic announce, or the first one once an interface is online
		if (!announced || millis() - last_announce_ms >= ANNOUNCE_INTERVAL_MS) {
			announce();
		}
		// After a TCP reconnect the remote side has lost the path
		if (tcp_impl && tcp_impl->connect_count() != tcp_seen_connects) {
			tcp_seen_connects = tcp_impl->connect_count();
			announce();
		}
	}

	diag.stage(madpico::STAGE_LINKS);
	if (millis() - last_link_check_ms > 2000) {
		last_link_check_ms = millis();
		enforce_link_limits();
		// NTP failed at boot: keep trying. An announce sent with the old
		// clock was likely dropped, so announce again once the time is right.
		if (!clock_synced && sync_clock() && node_destination) {
			announce();
		}
	}

	diag.stage(madpico::STAGE_SERIAL);
	handle_serial();
}

void loop() {
	// Anything thrown outside reticulum.loop() (which catches its own
	// exceptions) would otherwise end in std::terminate and a reset
	try {
		loop_work();
	}
	catch (const std::bad_alloc&) {
		Serial.printf("[diag] OUT OF MEMORY in stage %lu, free heap %d\n",
			(unsigned long)watchdog_hw->scratch[3], rp2040.getFreeHeap());
	}
	catch (const std::exception& e) {
		Serial.printf("[diag] Exception in stage %lu: %s\n",
			(unsigned long)watchdog_hw->scratch[3], e.what());
	}
	diag.stage(madpico::STAGE_IDLE);
	diag.update();
	rp2040.wdt_reset();

	// LED: on = online, slow blink = uplink ok but TCP down, fast blink = no uplink
	bool net_ok = madpico::net_up();
	bool tcp_ok = !tcp_impl || tcp_impl->connected();
	digitalWrite(LED_BUILTIN, (net_ok && tcp_ok) ? HIGH :
		net_ok ? (millis() / 1000) % 2 : (millis() / 200) % 2);
	delay(1);
}
