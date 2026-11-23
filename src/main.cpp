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
#include "FileTransfer.h"
#include "PicoEntropy.h"
#include "PicoLittleFS.h"
#include "PicoTCPClientInterface.h"
#include "PicoUDPInterface.h"

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
	if (clock_synced || WiFi.status() != WL_CONNECTED) return false;
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
#define LAST_ANNOUNCE_PATH "/last_announce"
static bool announced = false;  // last_announce_ms is valid

static void persist_last_announce() {
	if (!clock_synced) return;
	uint64_t t = (uint64_t)time(nullptr);
	fs::File f = LittleFS.open(LAST_ANNOUNCE_PATH, "w");
	if (!f) return;
	f.write((const uint8_t*)&t, sizeof(t));
	f.close();
}

// Restores the time of the last announce (needs real time), so that reboots
// don't cause extra announces
static void restore_last_announce() {
	if (!clock_synced) return;
	fs::File f = LittleFS.open(LAST_ANNOUNCE_PATH, "r");
	if (!f) return;
	uint64_t t = 0;
	bool ok = f.read((uint8_t*)&t, sizeof(t)) == sizeof(t);
	f.close();
	uint64_t now = (uint64_t)time(nullptr);
	if (!ok || t == 0 || t > now) return;
	uint64_t ago_ms = (now - t) * 1000ULL;
	if (ago_ms >= ANNOUNCE_INTERVAL_MS) return;
	// Unsigned wrap-around keeps "millis() - last_announce_ms" correct
	last_announce_ms = millis() - (uint32_t)ago_ms;
	announced = true;
	Serial.printf("[rns] Last announce was %lu min ago\n", (unsigned long)(ago_ms / 60000));
}

static bool auto_announce_allowed() {
	return !announced || millis() - last_announce_ms >= ANNOUNCE_MIN_GAP_MS;
}

static void announce() {
	if (!any_interface_online()) {
		// Not counted as sent, so we announce right after (re)connecting
		Serial.println("[rns] Announce postponed, no interface online");
		return;
	}
	node_destination.announce(RNS::Bytes(NODE_NAME));
	last_announce_ms = millis();
	announced = true;
	persist_last_announce();
	Serial.printf("[rns] Announce sent: %s \"%s\"\n",
		node_destination.hash().toHex().c_str(), NODE_NAME);
}

static void auto_announce(const char* reason) {
	if (auto_announce_allowed()) {
		announce();
	}
	else {
		Serial.printf("[rns] Skipping announce (%s), last one was %lu min ago\n",
			reason, (unsigned long)((millis() - last_announce_ms) / 60000));
	}
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

	char wifi[96];
#if STATUS_SHOW_NETWORK
	snprintf(wifi, sizeof(wifi), "%s, %d dBm, IP %s",
		WiFi.SSID().c_str(), WiFi.RSSI(), WiFi.localIP().toString().c_str());
#else
	snprintf(wifi, sizeof(wifi), "%d dBm", WiFi.RSSI());
#endif

	char page[1024];
	snprintf(page, sizeof(page),
		">%s status\n"
		"\n"
		"`!Address`!    %s\n"
		"`!Firmware`!   madpico %s\n"
		"`!Uptime`!     %lud %02lu:%02lu:%02lu\n"
		"`!Free heap`!  %d / %d bytes\n"
		"`!Flash (FS)`! %lu / %lu bytes used\n"
		"`!WiFi`!       %s\n"
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
		wifi,
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
	Serial.printf("Uptime %lus, WiFi %s (%d dBm), TCP %s (%lu connects), free heap %d, "
		"links %u active / %u pending (%lu total, %lu idle-closed, %lu limit-closed), "
		"pages %u, requests %lu, clock %s (rns time %lu)\n",
		(unsigned long)(millis() / 1000),
		WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "disconnected",
		WiFi.RSSI(),
		tcp_impl ? (tcp_impl->connected() ? "connected" : "disconnected") : "off",
		(unsigned long)(tcp_impl ? tcp_impl->connect_count() : 0),
		rp2040.getFreeHeap(),
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
				if (announced && millis() - last_announce_ms < ANNOUNCE_MIN_GAP_MS) {
					Serial.println("[rns] Note: transport nodes rate-limit frequent announces; "
						"too many get the node's announces dropped for a while");
				}
				announce();
				break;
			case 's': print_status(); break;
			case 'w': wifi_scan(); break;
			case 'c': WiFi.disconnect(); wifi_connect(20000); break;
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

	// The first join right after boot often fails (freshly initialised
	// CYW43), so try several rounds at startup
	for (int round = 0; round < 3 && !wifi_connect(15000); ++round) delay(1000);

	// Get real time before the first announce (see sync_clock())
	if (WiFi.status() == WL_CONNECTED) {
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
	restore_last_announce();
	tcp_seen_connects = tcp_impl ? tcp_impl->connect_count() : 0;
	if (any_interface_online()) auto_announce("boot");
}

void setup() {
	pinMode(LED_BUILTIN, OUTPUT);
	digitalWrite(LED_BUILTIN, HIGH);

	Serial.begin(115200);
	uint32_t start = millis();
	while (!Serial && millis() - start < 3000) delay(10);

	Serial.println();
	Serial.printf("=== madpico %s - NomadNet page node ===\n", FW_VERSION);
	try {
		setup_node();
	}
	catch (const std::exception& e) {
		Serial.printf("ERROR during startup: %s\n", e.what());
	}
	Serial.println("Serial commands: a = announce, s = status, w = WiFi scan, c = reconnect WiFi,");
	Serial.println("                 r = rewrite default pages + reboot");
}

void loop() {
	reticulum.loop();

	// WiFi reconnect (the interfaces restart themselves afterwards)
	if (WiFi.status() != WL_CONNECTED && millis() - last_wifi_attempt_ms > WIFI_RETRY_MS) {
		wifi_connect(10000);
	}

	if (node_destination && any_interface_online()) {
		// Periodic announce, or the first one once an interface is online
		if (!announced || millis() - last_announce_ms >= ANNOUNCE_INTERVAL_MS) {
			announce();
		}
		// After a TCP reconnect the remote side may have lost the path,
		// announce again (subject to the minimum gap)
		if (tcp_impl && tcp_impl->connect_count() != tcp_seen_connects) {
			tcp_seen_connects = tcp_impl->connect_count();
			auto_announce("TCP reconnect");
		}
	}

	if (millis() - last_link_check_ms > 2000) {
		last_link_check_ms = millis();
		enforce_link_limits();
		// NTP failed at boot: keep trying. An announce sent with the old
		// clock was likely dropped, so announce again once the time is right
		// (unless the persisted last announce is recent).
		if (!clock_synced && sync_clock() && node_destination) {
			announced = false;
			restore_last_announce();
			auto_announce("clock synced");
		}
	}

	handle_serial();

	// LED: on = online, slow blink = WiFi ok but TCP down, fast blink = no WiFi
	bool wifi_ok = (WiFi.status() == WL_CONNECTED);
	bool tcp_ok = !tcp_impl || tcp_impl->connected();
	digitalWrite(LED_BUILTIN, (wifi_ok && tcp_ok) ? HIGH :
		wifi_ok ? (millis() / 1000) % 2 : (millis() / 200) % 2);
	delay(1);
}
