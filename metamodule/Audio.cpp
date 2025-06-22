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

	if (inputChannels == 0) {
		n = generateNoise();
		inChannels = 1;
	} else {
		inChannels = inputChannels;
	}

	for (int i = 0; i < inChannels; i++) {
		if (!nInputBuffer[i].full()) {
			if (inputChannels == 0) {
				nInputFrame[i].samples[0] = n / 5.0f;
			} else if (inputChannels == 1) {
				// VCV: nInputFrame[i].samples[0] = input.getVoltage(i) / 5.0f;
				nInputFrame[i].samples[0] = input[0].getVoltage() / 5.0f;
			} else {
				// VCV: nInputFrame[i].samples[0] = input.getVoltage(0) / 5.0f;
				nInputFrame[i].samples[0] = input[i].getVoltage() / 5.0f;
			}
			nInputBuffer[i].push(nInputFrame[i]);
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

			switch(inChannels) {
				case 1:
					io.in[i][j] 		= v;
					io.in[1 + i][j] 	= v;
					io.in[2 + i][j] 	= v;
					io.in[3 + i][j] 	= v;
					io.in[4 + i][j] 	= v;
					io.in[5 + i][j] 	= v;
					break;
				case 2:
					io.in[i][j] 		= v;
					io.in[2 + i][j] 	= v;
					io.in[4 + i][j] 	= v;
					break;
				case 3:
					io.in[i * 2][j] 	= v;
					io.in[1 + i * 2][j] = v;
					break;
				default:
					io.in[i][j] 		= v;
			}
		}
	}
}

template<typename SrcT, typename BuffT, typename FrameT>
void resample_output(SrcT &outputSrc, BuffT &outputBuffer, FrameT &outputFrames, float internalSampleRate, float sampleRate) {
	outputSrc.setRates(internalSampleRate, sampleRate);
	int inLen = NUM_SAMPLES;
	int outLen = outputBuffer.capacity();
	outputSrc.process(outputFrames, &inLen, outputBuffer.endData(), &outLen);
	outputBuffer.endIncr(outLen);
}


template<typename BuffT>
void set_outputs(BuffT &outputBuffer, std::span<rack::engine::Output> output, float outputScale) {
// VCV: void set_outputs(auto &outputBuffer, rack::engine::Output &output, float outputScale) {
	if (!outputBuffer.empty()) {
		auto out = outputBuffer.shift();
		for (size_t i = 0; i < output.size(); i++) {
			output[i].setVoltage(out.samples[i] * 5.0f * outputScale);
			// VCV: output.setVoltage(out.samples[i] * 5.0f * outputScale, i);
		}
	}
}

void merge_outs(rainbow::IO &io, std::array<rack::dsp::Frame<1>, NUM_SAMPLES> &outputFrames1) {
	for (int i = 0; i < NUM_SAMPLES; i++) {
		outputFrames1[i].samples[0] = 0;
		for (int chan = 0; chan < NUM_CHANNELS; chan++) {
			outputFrames1[i].samples[0] += io.out[chan][i] / MAX_12BIT;
		}
	}
}

void Audio::ChannelProcess(rainbow::IO &io, std::span<rack::engine::Input, 6> input, std::span<rack::engine::Output, 1> output, rainbow::FilterBank &filterbank) {
	auto inChannels = populate_inputs(input);

	// Process buffer
	if (outputBuffer1.empty()) {
		route_inputs(io, inChannels);

		// Pass to filter
		filterbank.process_audio_block();

		// Convert output buffer
		for (int i = 0; i < NUM_SAMPLES; i++) {
			outputFrames1[i].samples[0] = 0;
			for (int chan = 0; chan < NUM_CHANNELS; chan++) {
				outputFrames1[i].samples[0] += io.out[chan][i] / MAX_12BIT;
			}
		}
		resample_output(outputSrc1, outputBuffer1, outputFrames1, internalSampleRate, sampleRate);
	}
	set_outputs(outputBuffer1, output, outputScale);
}

void Audio::ChannelProcess(rainbow::IO &io, std::span<rack::engine::Input, 6> input, std::span<rack::engine::Output, 2> output, rainbow::FilterBank &filterbank) {
	auto inChannels = populate_inputs(input);

	// Process buffer
	if (outputBuffer2.empty()) {
		route_inputs(io, inChannels);

		// Pass to filter
		filterbank.process_audio_block();

		// Convert output buffer
		for (int i = 0; i < NUM_SAMPLES; i++) {
			outputFrames2[i].samples[0] = 0;
			outputFrames2[i].samples[1] = 0;
			for (int chan = 0; chan < NUM_CHANNELS; chan++) {
				outputFrames2[i].samples[chan & 1] += io.out[chan][i] / MAX_12BIT;
			}
		}
		resample_output(outputSrc2, outputBuffer2, outputFrames2, internalSampleRate, sampleRate);
	}
	set_outputs(outputBuffer2, output, outputScale);
}

void Audio::ChannelProcess(rainbow::IO &io, std::span<rack::engine::Input, 6> input, std::span<rack::engine::Output, 6> output, rainbow::FilterBank &filterbank) {
	auto inChannels = populate_inputs(input);

	// Process buffer
	if (outputBuffer6.empty()) {
		route_inputs(io, inChannels);

		// Pass to filter
		filterbank.process_audio_block();

		// Convert output buffer
		for (int i = 0; i < NUM_SAMPLES; i++) {
			for (int chan = 0; chan < NUM_CHANNELS; chan++) {
				outputFrames6[i].samples[chan] = io.out[chan][i] / MAX_12BIT;
			}
		}
		resample_output(outputSrc2, outputBuffer2, outputFrames2, internalSampleRate, sampleRate);
	}
	set_outputs(outputBuffer6, output, outputScale);
}





