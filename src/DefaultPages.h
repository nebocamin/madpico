// SPDX-License-Identifier: Apache-2.0
//
// Pages written to flash on first boot (or with the serial command 'r').
// Afterwards they can be replaced with tools/madpico.py.
#pragma once

static const char DEFAULT_INDEX_PAGE[] =
	">madpico\n"
	"\n"
	"Hello from a `!Raspberry Pi Pico 2 W`!!\n"
	"\n"
	"This page is served by a microcontroller running `*microReticulum`*.\n"
	"Pages are stored in flash (LittleFS) under /pages.\n"
	"\n"
	"`F00a`_`[Node status`:/page/status.mu]`_`f\n"
	"`F00a`_`[About this project`:/page/about.mu]`_`f\n";

static const char DEFAULT_ABOUT_PAGE[] =
	">About madpico\n"
	"\n"
	"madpico is a NomadNet compatible page node that runs entirely on a\n"
	"Raspberry Pi Pico 2 W. There is no Linux and no Python interpreter,\n"
	"just a microcontroller with 520 KB of RAM and 4 MB of flash.\n"
	"\n"
	">>Hardware and software\n"
	"\n"
	"- MCU: RP2350, 2x Cortex-M33 at 150 MHz\n"
	"- Radio: CYW43439, 2.4 GHz WiFi\n"
	"- Stack: microReticulum (C++ port of Reticulum)\n"
	"- Framework: arduino-pico (Earle Philhower)\n"
	"- Storage: LittleFS in internal flash (1 MB)\n"
	"- Interface: TCP client to rnsd (TCPServerInterface), optional UDP broadcast\n"
	"\n"
	">>Cryptography\n"
	"\n"
	"Links are negotiated with X25519 and signed with Ed25519, exactly like\n"
	"Python Reticulum. Random numbers come from the RP2350 hardware TRNG.\n"
	"Establishing a link costs the Pico roughly 100 to 200 ms of CPU time.\n"
	"\n"
	">>Limits\n"
	"\n"
	"- Static pages from flash plus built-in dynamic pages\n"
	"  (no executable scripts like NomadNet / rns-page-node)\n"
	"- Pages up to 16 KB, no bz2 compression for resources\n"
	"- No file downloads (/file/...) yet\n"
	"\n"
	"This page is intentionally longer than a single Reticulum packet, so it\n"
	"is transferred as a resource in several parts.\n"
	"\n"
	"`F00a`_`[Back`:/page/index.mu]`_`f\n";
