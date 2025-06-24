#include "../../../metamodule/firmware/metamodule-plugin-sdk/debug_raw.h"
#include "../src/Rainbow.hpp"

using namespace rainbow;

float Audio::generateNoise() {
	float nO;
	switch (noiseSelected) {
		case 0:
			nO = brown.next() * 10.0f - 5.0f;
			break;
		case 1:
			nO = pink.next() * 10.0f - 5.0f;
			break;
		case 2:
			nO = white.next() * 10.0f - 5.0f;
			break;
		default:
			nO = pink.next() * 10.0f - 5.0f;
	}
	return nO;
}

int Audio::populate_inputs(std::span<rack::engine::Input> input) {
	int inChannels;
	float n = 0.0f;

	//typ: 52ns, max 230ns, avg 58ns
	inputChannels = 0;
	for (auto i = 0u; i < input.size(); i++) {
		if (input[i].isConnected())
			inputChannels = i + 1;
	}

	if (inputChannels == 0) {
		n = generateNoise();
		inChannels = 1;
	} else {
		inChannels = inputChannels;
	}

	const auto in0 = input[0].getVoltage();

	//typ: 300ns, max 500ns, avg 318ns
	// 6 channels: avg 1.25us = 6.0% load
	// 3 channels: avg 0.67us
	// 2 channels: avg 0.5us
	// 1 channel: avg 0.3ns = 1.4% load
	for (int i = 0; i < inChannels; i++) {
		if (!nInputBuffer[i].full()) {
			if (inputChannels == 0) {
				nInputFrame[i].samples[0] = n;
			} else if (inputChannels == 1) {
				nInputFrame[i].samples[0] = in0;
			} else {
				nInputFrame[i].samples[0] = input[i].getVoltage();
			}
			nInputBuffer[i].push(nInputFrame[i]);
		}
	}

	return inChannels;
}

void Audio::route_inputs(rainbow::IO &io, int inChannels) {
	// 1 input: 1us per 32-block = 0.031us/sample = 0.15% load
	// 6 inputs: 10us per 32-block = 0.312us/sample = 1.5% load
	for (int i = 0; i < inChannels; i++) {
		// Each channel, avg 1.1us per block of 32 samples = 0.034us/sample
		nInputSrc[i].setRates(sampleRate, internalSampleRate);

		int inLen = nInputBuffer[i].size();
		int outLen = NUM_SAMPLES;
		nInputSrc[i].process(nInputBuffer[i].startData(), &inLen, nInputFrames[i], &outLen);
		nInputBuffer[i].startIncr(inLen);

		for (int j = 0; j < NUM_SAMPLES; j++) {
			constexpr static int32_t I_MIN_24BIT = -16777216;
			constexpr static int32_t I_MAX_24BIT = 16777215;
			int32_t v =
				std::clamp<int32_t>(nInputFrames[i][j].samples[0] * (MAX_12BIT / 5.f), I_MIN_24BIT, I_MAX_24BIT);

			switch (inChannels) {
				case 1:
					io.in[i][j] = v;
					io.in[1 + i][j] = v;
					io.in[2 + i][j] = v;
					io.in[3 + i][j] = v;
					io.in[4 + i][j] = v;
					io.in[5 + i][j] = v;
					break;
				case 2:
					io.in[i][j] = v;
					io.in[2 + i][j] = v;
					io.in[4 + i][j] = v;
					break;
				case 3:
					io.in[i * 2][j] = v;
					io.in[1 + i * 2][j] = v;
					break;
				default:
					io.in[i][j] = v;
			}
		}
	}
}

// Convert output buffer: 6 -> 1
void route_outputs(rainbow::IO &io, std::span<rack::dsp::Frame<1>, NUM_SAMPLES> outputFrames1) {
	for (int i = 0; i < NUM_SAMPLES; i++) {
		outputFrames1[i].samples[0] = 0;
		for (int chan = 0; chan < NUM_CHANNELS; chan++) {
			outputFrames1[i].samples[0] += io.out[chan][i] / Audio::MAX_12BIT;
		}
	}
}

// Convert output buffer: 6 -> 2
void route_outputs(rainbow::IO &io, std::span<rack::dsp::Frame<2>, NUM_SAMPLES> outputFrames2) {
	for (int i = 0; i < NUM_SAMPLES; i++) {
		outputFrames2[i].samples[0] = 0;
		outputFrames2[i].samples[1] = 0;
		for (int chan = 0; chan < NUM_CHANNELS; chan++) {
			outputFrames2[i].samples[chan & 1] += io.out[chan][i] / Audio::MAX_12BIT;
		}
	}
}

// Convert output buffer: 6 -> 6
void route_outputs(rainbow::IO &io, std::span<rack::dsp::Frame<6>, NUM_SAMPLES> outputFrames6) {
	for (int chan = 0; chan < NUM_CHANNELS; chan++) {
		for (int i = 0; i < NUM_SAMPLES; i++) {
			outputFrames6[i].samples[chan] = io.out[chan][i] / Audio::MAX_12BIT;
		}
	}
}

void resample_output(
	auto &outputSrc, auto &outputBuffer, auto &outputFrames, float internalSampleRate, float sampleRate) {
	outputSrc.setRates(internalSampleRate, sampleRate);
	int inLen = NUM_SAMPLES;
	int outLen = outputBuffer.capacity();
	outputSrc.process(outputFrames, &inLen, outputBuffer.endData(), &outLen);
	outputBuffer.endIncr(outLen);
}

void set_outputs(auto &outputBuffer, std::span<rack::engine::Output> output, float outputScale, int outputChannels) {
	if (!outputBuffer.empty()) {
		auto out = outputBuffer.shift();
		for (size_t i = 0; i < outputChannels; i++) {
			output[i].setVoltage(out.samples[i] * 5.0f * outputScale);
		}
	}
}

void channel_process_resample(auto &outputSrc,
							  auto &outputBuffer,
							  auto &outputFrames,
							  rainbow::IO &io,
							  std::span<rack::engine::Input> input,
							  std::span<rack::engine::Output> output,
							  rainbow::FilterBank &filterbank,
							  Audio *audio) {
	// 0 input jacks: 0.71us sample + 18us block  29%
	// 1 input jacks: 0.60us sample + 28us block
	// 2 input jacks: 0.75us sample + 28us block
	// 3 input jacks:               + 32us block
	// 4 input jacks:               + 34us block
	// 5 input jacks:               + 35us block
	// 6 input jacks: 1.4us sample + 377us block 39%

	auto inChannels = audio->populate_inputs(input);

	if (outputBuffer.empty()) {
		audio->route_inputs(io, inChannels);

		filterbank.process_audio_block();

		route_outputs(io, outputFrames);

		resample_output(outputSrc, outputBuffer, outputFrames, audio->internalSampleRate, audio->sampleRate);
	}

	set_outputs(outputBuffer, output, audio->outputScale, audio->outputChannels);
}

void Audio::channel_process_no_resample(rainbow::IO &io,
										std::span<rack::engine::Input> input,
										std::span<rack::engine::Output> output,
										rainbow::FilterBank &filterbank) {

	// no jacks: 26% load +1% each jack => 32% 6 jacks
	// Any jack configuration: 0.56us sample; 20.6us block
	constexpr static int32_t I_MIN_24BIT = -16777216;
	constexpr static int32_t I_MAX_24BIT = 16777215;

	// Count inputs
	inputChannels = 0;
	for (auto i = 0u; i < input.size(); i++) {
		if (input[i].isConnected())
			inputChannels = i + 1;
	}

	int inChannels = std::max(1, inputChannels);

	// Route input jacks to filter inputs
	if (inChannels == 1) {
		auto n = inputChannels == 0 ? generateNoise() : input[0].getVoltage();
		int32_t v = std::clamp<int32_t>(n * (Audio::MAX_12BIT / 5.f), I_MIN_24BIT, I_MAX_24BIT);

		io.in[0][block_ctr] = v;
		io.in[1][block_ctr] = v;
		io.in[2][block_ctr] = v;
		io.in[3][block_ctr] = v;
		io.in[4][block_ctr] = v;
		io.in[5][block_ctr] = v;

	} else if (inChannels == 2) {
		for (auto i = 0; i < inChannels; i++) {
			auto n = input[i].getVoltage();
			int32_t v = std::clamp<int32_t>(n * (Audio::MAX_12BIT / 5.f), I_MIN_24BIT, I_MAX_24BIT);

			io.in[0 + i][block_ctr] = v;
			io.in[2 + i][block_ctr] = v;
			io.in[4 + i][block_ctr] = v;
		}

	} else if (inChannels == 3) {
		for (auto i = 0; i < inChannels; i++) {
			auto n = input[i].getVoltage();
			int32_t v = std::clamp<int32_t>(n * (Audio::MAX_12BIT / 5.f), I_MIN_24BIT, I_MAX_24BIT);

			io.in[0 + i * 2][block_ctr] = v;
			io.in[1 + i * 2][block_ctr] = v;
		}

	} else {
		for (auto i = 0; i < inChannels; i++) {
			auto n = input[i].getVoltage();
			int32_t v = std::clamp<int32_t>(n * (Audio::MAX_12BIT / 5.f), I_MIN_24BIT, I_MAX_24BIT);
			io.in[i][block_ctr] = v;
		}
	}

	// Route filter outputs to output jacks
	if (outputChannels == 1) {
		// 6 => 1
		float out = 0;
		for (int chan = 0; chan < NUM_CHANNELS; chan++) {
			out += io.out[chan][block_ctr] / Audio::MAX_12BIT;
		}
		output[0].setVoltage(out * outputScale);

	} else if (outputChannels == 2) {
		// 6 => 2
		float channel[2] = {0, 0};
		for (int chan = 0; chan < NUM_CHANNELS; chan++) {
			channel[chan & 1] += io.out[chan][block_ctr] / Audio::MAX_12BIT;
		}
		output[0].setVoltage(channel[0] * outputScale);
		output[1].setVoltage(channel[1] * outputScale);

	} else if (outputChannels == 6) {
		// 6 => 6
		for (auto chan = 0u; chan < NUM_CHANNELS; chan++) {
			auto sample = io.out[chan][block_ctr] / Audio::MAX_12BIT;
			output[chan].setVoltage(sample * outputScale);
		}
	}

	block_ctr++;

	// Process blocks
	if (block_ctr >= NUM_SAMPLES) {
		block_ctr = 0;
		filterbank.process_audio_block();
	}
}

void Audio::ChannelProcess(rainbow::IO &io,
						   std::span<rack::engine::Input> input,
						   std::span<rack::engine::Output> output,
						   rainbow::FilterBank &filterbank) {

	if (internalSampleRate == sampleRate) {
		channel_process_no_resample(io, input, output, filterbank);

	} else {

		if (outputChannels == 1)
			channel_process_resample(outputSrc1, outputBuffer1, outputFrames1, io, input, output, filterbank, this);

		else if (outputChannels == 2)
			channel_process_resample(outputSrc2, outputBuffer2, outputFrames2, io, input, output, filterbank, this);

		else if (outputChannels == 6)
			channel_process_resample(outputSrc6, outputBuffer6, outputFrames6, io, input, output, filterbank, this);
	}
}
