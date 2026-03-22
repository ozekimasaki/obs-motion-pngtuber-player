#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct MptAudioInputDevice {
	uint32_t index = 0;
	std::string name;
	std::string host_api;
	std::string label;
	std::string identity_json;
};

struct MptAudioCapture;

typedef void (*MptAudioInputCallback)(const int16_t *samples, size_t sample_count, uint16_t channels, uint32_t sample_rate,
				      void *userdata);

std::vector<MptAudioInputDevice> mpt_audio_backend_enumerate_input_devices();
bool mpt_audio_backend_resolve_input_device(const std::string &identity_json, long long fallback_index,
					    uint32_t *out_index);
bool mpt_audio_backend_start_input_capture(const std::string &identity_json, long long fallback_index, MptAudioInputCallback callback,
					   void *userdata, MptAudioCapture **out_capture, std::string &error);
void mpt_audio_backend_stop_input_capture(MptAudioCapture *capture);
