#include "mpt-audio-backend.h"

#include "motionpngtuber-native.h"

#include <string>
#include <vector>

#ifdef _WIN32
#include <mmeapi.h>
#endif

namespace {

#ifdef _WIN32

static std::string wide_to_utf8(const wchar_t *text)
{
	if (!text || !*text)
		return std::string();

	int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	if (needed <= 0)
		return std::string();

	std::string out(static_cast<size_t>(needed), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
	if (!out.empty() && out.back() == '\0')
		out.pop_back();
	return out;
}

static std::string build_identity_json(uint32_t index, const std::string &name, const char *host_api)
{
	std::string out;
	obs_data_t *identity = obs_data_create();
	if (!identity)
		return out;

	obs_data_set_int(identity, "index", static_cast<long long>(index));
	obs_data_set_string(identity, "name", name.c_str());
	obs_data_set_string(identity, "hostapi", host_api);
	const char *json = obs_data_get_json(identity);
	if (json)
		out = json;
	obs_data_release(identity);
	return out;
}

#endif

} // namespace

std::vector<MptAudioInputDevice> mpt_audio_backend_enumerate_input_devices()
{
	std::vector<MptAudioInputDevice> devices;

#ifdef _WIN32
	UINT count = waveInGetNumDevs();
	devices.reserve(static_cast<size_t>(count));
	for (UINT idx = 0; idx < count; ++idx) {
		WAVEINCAPSW caps = {};
		if (waveInGetDevCapsW(idx, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
			continue;

		MptAudioInputDevice device;
		device.index = idx;
		device.name = wide_to_utf8(caps.szPname);
		device.host_api = "MME";
		device.label = std::to_string(idx) + " " + device.name + " [" + device.host_api + "]";
		device.identity_json = build_identity_json(device.index, device.name, device.host_api.c_str());
		devices.push_back(std::move(device));
	}
#endif

	return devices;
}

bool mpt_audio_backend_resolve_input_device(const std::string &identity_json, long long fallback_index, uint32_t *out_index)
{
	if (out_index)
		*out_index = 0;

#ifdef _WIN32
	UINT count = waveInGetNumDevs();
	if (count == 0)
		return false;

	std::string desired_name;
	long long desired_index = -1;
	if (!identity_json.empty()) {
		obs_data_t *identity = obs_data_create_from_json(identity_json.c_str());
		if (identity) {
			const char *name = obs_data_get_string(identity, "name");
			if (name)
				desired_name = name;
			desired_index = obs_data_get_int(identity, "index");
			obs_data_release(identity);
		}
	}

	if (!desired_name.empty()) {
		for (UINT idx = 0; idx < count; ++idx) {
			WAVEINCAPSW caps = {};
			if (waveInGetDevCapsW(idx, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
				continue;
			if (wide_to_utf8(caps.szPname) == desired_name) {
				if (out_index)
					*out_index = idx;
				return true;
			}
		}
	}

	if (desired_index >= 0 && desired_index < static_cast<long long>(count)) {
		if (out_index)
			*out_index = static_cast<uint32_t>(desired_index);
		return true;
	}

	if (fallback_index >= 0 && fallback_index < static_cast<long long>(count)) {
		if (out_index)
			*out_index = static_cast<uint32_t>(fallback_index);
		return true;
	}

	if (out_index)
		*out_index = 0;
	return true;
#else
	UNUSED_PARAMETER(identity_json);
	UNUSED_PARAMETER(fallback_index);
	return false;
#endif
}
