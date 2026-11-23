// SPDX-License-Identifier: Apache-2.0
//
// Crash diagnostics: the reset reason, plus uptime, lowest free heap and the
// last loop stage of the previous run, kept in the watchdog scratch registers
// 0-3 (they survive soft/watchdog resets, but not a power-on reset).
#pragma once

#include <Arduino.h>
#include <hardware/structs/watchdog.h>

namespace madpico {

enum DiagStage : uint32_t {
	STAGE_SETUP = 1,
	STAGE_RETICULUM,
	STAGE_WIFI,
	STAGE_ANNOUNCE,
	STAGE_LINKS,
	STAGE_SERIAL,
	STAGE_IDLE,
};

class Diagnostics {
public:
	static constexpr uint32_t MAGIC = 0x4D414450;  // "MADP"

	// Call once at boot, before update()
	void report_previous_run() {
		static const char* reasons[] = {"unknown", "power-on", "RUN pin", "software",
			"watchdog", "debug", "glitch", "brownout"};
		unsigned r = (unsigned)rp2040.getResetReason();
		Serial.printf("[diag] Reset reason: %s\n", r < 8 ? reasons[r] : "?");
		// The bootloader may reuse the scratch registers (e.g. after flashing),
		// so only trust plausible values
		if (watchdog_hw->scratch[0] == MAGIC && watchdog_hw->scratch[3] >= STAGE_SETUP &&
				watchdog_hw->scratch[3] <= STAGE_IDLE && watchdog_hw->scratch[2] < 1024 * 1024) {
			Serial.printf("[diag] Previous run: uptime %lus, lowest free heap %lu bytes, last stage %lu\n",
				(unsigned long)watchdog_hw->scratch[1], (unsigned long)watchdog_hw->scratch[2],
				(unsigned long)watchdog_hw->scratch[3]);
		}
		watchdog_hw->scratch[0] = MAGIC;
		watchdog_hw->scratch[1] = 0;
		watchdog_hw->scratch[2] = 0xFFFFFFFF;
		watchdog_hw->scratch[3] = STAGE_SETUP;
	}

	void stage(DiagStage s) { watchdog_hw->scratch[3] = s; }

	// Cheap enough to call every loop iteration
	void update() {
		uint32_t free_heap = (uint32_t)rp2040.getFreeHeap();
		if (free_heap < _min_heap) {
			_min_heap = free_heap;
			watchdog_hw->scratch[2] = free_heap;
		}
		watchdog_hw->scratch[1] = millis() / 1000;
	}

	uint32_t min_heap() const { return _min_heap; }

private:
	uint32_t _min_heap = 0xFFFFFFFF;
};

} // namespace madpico
