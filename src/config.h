// SPDX-License-Identifier: Apache-2.0
//
// Build-time configuration. Local settings (WiFi credentials, TCP target,
// node name, ...) live in src/secrets.h, which is git-ignored. Every value
// below can be overridden there.
#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "src/secrets.h not found - using src/secrets.h.example. Copy it to src/secrets.h and adjust it."
#include "secrets.h.example"
#endif

#define FW_VERSION "0.1.0"

// --- Node -------------------------------------------------------------------
#ifndef NODE_NAME
#define NODE_NAME "madpico"
#endif
#define APP_NAME   "nomadnetwork"
#define APP_ASPECT "node"

// Announce interval, same default as rns-page-node (6 hours)
#ifndef ANNOUNCE_INTERVAL_MS
#define ANNOUNCE_INTERVAL_MS (360UL * 60UL * 1000UL)
#endif
// Minimum gap between automatic announces (boot, TCP reconnect). Transport
// nodes rate-limit announces per destination (typically ~1 per hour) and stop
// rebroadcasting them for a while if a destination announces too often. The
// time of the last announce is persisted, so reboots respect the gap too.
// Clients can still reach the node in between via path requests.
#ifndef ANNOUNCE_MIN_GAP_MS
#define ANNOUNCE_MIN_GAP_MS (60UL * 60UL * 1000UL)
#endif

// --- Interfaces -------------------------------------------------------------
// TCP client to a TCPServerInterface (e.g. rnsd or a public transport node)
#ifndef USE_TCP
#define USE_TCP 1
#endif
#if USE_TCP && !defined(TCP_TARGET_HOST)
#error "USE_TCP is enabled but TCP_TARGET_HOST is not defined (see src/secrets.h.example)"
#endif
#ifndef TCP_TARGET_PORT
#define TCP_TARGET_PORT 4242
#endif

// UDP broadcast on the local network (compatible with rnsd's UDPInterface)
#ifndef USE_UDP
#define USE_UDP 0
#endif
#ifndef UDP_PORT
#define UDP_PORT 4242
#endif

#ifndef WIFI_RETRY_MS
#define WIFI_RETRY_MS (30UL * 1000UL)
#endif

// NTP servers. Real time is required: announces carry an emission timestamp
// and Python RNS transport nodes drop announces that aren't newer than the
// last one they've seen from a destination.
#ifndef NTP_SERVER_1
#define NTP_SERVER_1 "pool.ntp.org"
#endif
#ifndef NTP_SERVER_2
#define NTP_SERVER_2 "time.cloudflare.com"
#endif

// --- Pages ------------------------------------------------------------------
#define PAGES_DIR        "/pages"
#define IDENTITY_PATH    "/node_identity"
#define STATUS_PAGE_PATH "/page/status.mu"
// Largest page that is served/accepted for upload. Pages are sent as a single
// in-memory Resource, which needs several copies of the data in RAM.
#ifndef MAX_PAGE_SIZE
#define MAX_PAGE_SIZE (16 * 1024)
#endif

// Show WiFi SSID and local IP on the (public) status page?
#ifndef STATUS_SHOW_NETWORK
#define STATUS_SHOW_NETWORK 0
#endif

// --- Resource limits --------------------------------------------------------
// Upper bounds for the in-RAM tables (oldest entries are evicted)
#ifndef KNOWN_DESTINATIONS_MAX
#define KNOWN_DESTINATIONS_MAX 100
#endif
#ifndef PATH_TABLE_MAX
#define PATH_TABLE_MAX 100
#endif

// Each active link costs roughly 7 KB of RAM. Crawlers on public networks
// tend to open links and keep them alive with keepalives only.
#ifndef MAX_ACTIVE_LINKS
#define MAX_ACTIVE_LINKS 16
#endif
// Links without any data (keepalives don't count) for this long are closed.
// NomadNet clients transparently re-establish the link on the next request.
#ifndef LINK_IDLE_TIMEOUT_S
#define LINK_IDLE_TIMEOUT_S 300
#endif
