// SPDX-License-Identifier: Apache-2.0
//
// Hardware entropy for the rweather/attermann Crypto RNG on RP2040/RP2350.
//
// The Crypto library knows no hardware random source for RP2xxx ("no hardware
// random number source detected for this platform"). Without one, the RNG
// starts from the same fixed state after every boot, so every device would
// generate identical keys. This feeds get_rand_32() from the Pico SDK into
// the RNG (RP2350: TRNG + ROSC + timer; RP2040: ROSC + timer).
#pragma once

#include <Arduino.h>
#include <RNG.h>
#include <NoiseSource.h>
#include <pico/rand.h>

class PicoTrngNoiseSource : public NoiseSource {
public:
	bool calibrating() const override { return false; }

	void stir() override {
		uint32_t buf[8];
		for (auto& w : buf) w = get_rand_32();
		// RP2350 TRNG: credit full entropy (256 bits per 32 bytes)
		output(reinterpret_cast<const uint8_t*>(buf), sizeof(buf), sizeof(buf) * 8);
		memset(buf, 0, sizeof(buf));
	}
};

// Must be called after RNS::Reticulum has been constructed (its constructor
// calls RNG.begin()) and before any key is generated. RNG.loop(), which is
// called from Reticulum::loop(), keeps stirring the source afterwards.
inline void pico_seed_rng() {
	static PicoTrngNoiseSource source;
	static bool added = false;
	if (!added) {
		RNG.addNoiseSource(source);
		added = true;
	}
	for (int i = 0; i < 4; ++i) source.stir();
}
