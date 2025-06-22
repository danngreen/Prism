#pragma once
#include <array>
#include <cstdint>

namespace MetaModule::PolyPolyfill {

template<int MAX_CHANNELS>
struct Port {
	std::array<float, MAX_CHANNELS> voltages = {};
	uint8_t channels = 0;

	void setVoltage(float v, uint8_t channel = 0) {
		if (channel < MAX_CHANNELS)
			voltages[channel] = v;
	}

	float getVoltage(uint8_t chan = 0) const {
		return (chan < MAX_CHANNELS) ? voltages[chan] : 0.f;
	}

	void setChannels(uint8_t channels) {
		// If disconnected, keep the number of channels at 0.
		if (this->channels == 0) {
			return;
		}
		// Set higher channel voltages to 0
		for (uint8_t c = channels; c < this->channels; c++) {
			voltages[c] = 0.f;
		}
		// Don't allow caller to set port as disconnected
		if (channels == 0) {
			channels = 1;
		}
		this->channels = channels;
	}

	int getChannels() const {
		return channels;
	}

	bool isConnected() const {
		return channels > 0;
	}
};

template<int MAX_CHANNELS>
struct Output : Port<MAX_CHANNELS> {};

template<int MAX_CHANNELS>
struct Input : Port<MAX_CHANNELS> {};

}
