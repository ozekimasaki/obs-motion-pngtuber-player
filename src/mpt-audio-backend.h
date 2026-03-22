#pragma once

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

std::vector<MptAudioInputDevice> mpt_audio_backend_enumerate_input_devices();
bool mpt_audio_backend_resolve_input_device(const std::string &identity_json, long long fallback_index,
					    uint32_t *out_index);
