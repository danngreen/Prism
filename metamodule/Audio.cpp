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

int Audio::populate_inputs(std::span<rack::engine::Input, 6> input) {
	// VCV: int Audio::populate_inputs(rack::engine::Input &input) {
	int inChannels;
	float n = 0.0f;

	inputChannels = 0;
	for (auto i = 0; i < input.size(); i++) {
		if (input[i].isConnected())
			inputChannels = i + 1;
	}

	if (inputChannels == 0) {
		n = generateNoise();
		inChannels = 1;
	} else {
		inChannels = inputChannels;
	}

	for (int i = 0; i < inChannels; i++) {
		if (!nInputBuffer[i].full()) {
			dsp::Frame<1> sample;
			if (inputChannels == 0) {
				sample.samples[0] = n / 5.0f;
			} else if (inputChannels == 1) {
				// VCV: nInputFrame[i].samples[0] = input.getVoltage(i) / 5.0f;
				sample.samples[0] = input[0].getVoltage() / 5.0f;
			} else {
				// VCV: nInputFrame[i].samples[0] = input.getVoltage(0) / 5.0f;
				sample.samples[0] = input[i].getVoltage() / 5.0f;
			}
			nInputBuffer[i].push(sample);
		}
	}
	return inChannels;
}

void Audio::route_inputs(rainbow::IO &io, int inChannels) {
	for (int i = 0; i < inChannels; i++) {
		nInputSrc[i].setRates(sampleRate, internalSampleRate);

		int inLen = nInputBuffer[i].size();
		int outLen = NUM_SAMPLES;
		nInputSrc[i].process(nInputBuffer[i].startData(), &inLen, nInputFrames[i], &outLen);
		nInputBuffer[i].startIncr(inLen);

		for (int j = 0; j < NUM_SAMPLES; j++) {
			int32_t v = std::clamp(nInputFrames[i][j].samples[0] * MAX_12BIT, MIN_12BIT, MAX_12BIT);

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
	for (int i = 0; i < NUM_SAMPLES; i++) {
		for (int chan = 0; chan < NUM_CHANNELS; chan++) {
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

void set_outputs(auto &outputBuffer, std::span<rack::engine::Output> output, float outputScale) {
	// VCV: void set_outputs(auto &outputBuffer, rack::engine::Output &output, float outputScale) {
	if (!outputBuffer.empty()) {
		auto out = outputBuffer.shift();
		for (size_t i = 0; i < output.size(); i++) {
			output[i].setVoltage(out.samples[i] * 5.0f * outputScale);
			// VCV: output.setVoltage(out.samples[i] * 5.0f * outputScale, i);
		}
	}
}

void channel_process(auto &outputSrc,
					 auto &outputBuffer,
					 auto &outputFrames,
					 rainbow::IO &io,
					 std::span<rack::engine::Input, 6> input,
					 std::span<rack::engine::Output> output,
					 rainbow::FilterBank &filterbank,
					 Audio *audio) {

	auto inChannels = audio->populate_inputs(input);

	if (outputBuffer.empty()) {
		audio->route_inputs(io, inChannels);

		filterbank.process_audio_block();

		route_outputs(io, outputFrames);

		resample_output(outputSrc, outputBuffer, outputFrames, audio->internalSampleRate, audio->sampleRate);
	}
	set_outputs(outputBuffer, output, audio->outputScale);
}

void Audio::ChannelProcess(rainbow::IO &io,
						   std::span<rack::engine::Input, 6> input,
						   std::span<rack::engine::Output, 1> output,
						   rainbow::FilterBank &filterbank) {
	channel_process(outputSrc1, outputBuffer1, outputFrames1, io, input, output, filterbank, this);
}

void Audio::ChannelProcess(rainbow::IO &io,
						   std::span<rack::engine::Input, 6> input,
						   std::span<rack::engine::Output, 2> output,
						   rainbow::FilterBank &filterbank) {
	channel_process(outputSrc2, outputBuffer2, outputFrames2, io, input, output, filterbank, this);
}

void Audio::ChannelProcess(rainbow::IO &io,
						   std::span<rack::engine::Input, 6> input,
						   std::span<rack::engine::Output, 6> output,
						   rainbow::FilterBank &filterbank) {
	channel_process(outputSrc6, outputBuffer6, outputFrames6, io, input, output, filterbank, this);
}
