#include "mpt-audio-backend.h"

#include "motionpngtuber-native.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <mmeapi.h>
#else
extern "C" {
#include <portaudio.h>
}
#endif

struct MptAudioCapture {
#ifdef _WIN32
	HWAVEIN wave_in = nullptr;
	uint16_t channels = 1;
	uint32_t sample_rate = 44100;
	MptAudioInputCallback callback = nullptr;
	void *userdata = nullptr;
	std::array<std::vector<int16_t>, 3> storage {};
	std::array<WAVEHDR, 3> headers {};
#else
	PaStream *stream = nullptr;
	uint16_t channels = 1;
	uint32_t sample_rate = 44100;
	MptAudioInputCallback callback = nullptr;
	void *userdata = nullptr;
#endif
};

namespace {

struct AudioFormatCandidate {
	uint16_t channels;
	uint32_t sample_rate;
};

static const AudioFormatCandidate k_audio_format_candidates[] = {
	{1, 44100},
	{1, 48000},
	{2, 44100},
	{2, 48000},
};

static std::string build_identity_json(uint32_t index, const std::string &name, const char *host_api)
{
	std::string out;
	obs_data_t *identity = obs_data_create();
	if (!identity)
		return out;

	obs_data_set_int(identity, "index", static_cast<long long>(index));
	obs_data_set_string(identity, "name", name.c_str());
	obs_data_set_string(identity, "hostapi", host_api ? host_api : "");
	const char *json = obs_data_get_json(identity);
	if (json)
		out = json;
	obs_data_release(identity);
	return out;
}

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

static bool get_wavein_device_name(UINT idx, std::string &name_out)
{
	WAVEINCAPSW caps = {};
	if (waveInGetDevCapsW(idx, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
		return false;
	name_out = wide_to_utf8(caps.szPname);
	return true;
}

static void dispatch_audio_samples(MptAudioCapture *capture, const int16_t *samples, size_t sample_count)
{
	if (!capture || !capture->callback || !samples || sample_count == 0)
		return;
	capture->callback(samples, sample_count, capture->channels, capture->sample_rate, capture->userdata);
}

static void CALLBACK wave_in_callback(HWAVEIN hwi, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2)
{
	UNUSED_PARAMETER(hwi);
	UNUSED_PARAMETER(param2);
	if (msg != WIM_DATA || !instance || !param1)
		return;

	auto *capture = reinterpret_cast<MptAudioCapture *>(instance);
	auto *header = reinterpret_cast<WAVEHDR *>(param1);
	if (!capture || !header)
		return;

	auto requeue_buffer = [&](WAVEHDR *queued_header) {
		if (!capture->wave_in)
			return;
		waveInAddBuffer(capture->wave_in, queued_header, sizeof(WAVEHDR));
	};

	if (header->dwBytesRecorded == 0) {
		requeue_buffer(header);
		return;
	}

	size_t sample_count = header->dwBytesRecorded / sizeof(int16_t);
	dispatch_audio_samples(capture, reinterpret_cast<const int16_t *>(header->lpData), sample_count);
	requeue_buffer(header);
}

#else

static std::mutex g_portaudio_mutex;
static size_t g_portaudio_refcount = 0;

static std::string portaudio_error_string(PaError error_code)
{
	const char *text = Pa_GetErrorText(error_code);
	return text && *text ? std::string(text) : std::string("PortAudio error");
}

static bool portaudio_acquire(std::string &error)
{
	std::lock_guard<std::mutex> lock(g_portaudio_mutex);
	if (g_portaudio_refcount == 0) {
		PaError err = Pa_Initialize();
		if (err != paNoError) {
			error = portaudio_error_string(err);
			return false;
		}
	}
	++g_portaudio_refcount;
	return true;
}

static void portaudio_release()
{
	std::lock_guard<std::mutex> lock(g_portaudio_mutex);
	if (g_portaudio_refcount == 0)
		return;
	--g_portaudio_refcount;
	if (g_portaudio_refcount == 0)
		Pa_Terminate();
}

static bool get_portaudio_device_details(PaDeviceIndex index, std::string &name_out, std::string &host_api_out)
{
	const PaDeviceInfo *device_info = Pa_GetDeviceInfo(index);
	if (!device_info || device_info->maxInputChannels <= 0)
		return false;

	name_out = device_info->name ? device_info->name : "";
	const PaHostApiInfo *host_api_info = Pa_GetHostApiInfo(device_info->hostApi);
	host_api_out = host_api_info && host_api_info->name ? host_api_info->name : "";
	return true;
}

static int portaudio_input_callback(const void *input, void *output, unsigned long frame_count,
				    const PaStreamCallbackTimeInfo *time_info, PaStreamCallbackFlags status_flags, void *user_data)
{
	UNUSED_PARAMETER(output);
	UNUSED_PARAMETER(time_info);
	UNUSED_PARAMETER(status_flags);

	auto *capture = reinterpret_cast<MptAudioCapture *>(user_data);
	if (!capture || !capture->callback || !input)
		return paContinue;

	size_t sample_count = static_cast<size_t>(frame_count) * capture->channels;
	capture->callback(reinterpret_cast<const int16_t *>(input), sample_count, capture->channels, capture->sample_rate,
			 capture->userdata);
	return paContinue;
}

#endif

static bool parse_identity_json(const std::string &identity_json, std::string &name_out, std::string &host_api_out,
				long long &index_out)
{
	name_out.clear();
	host_api_out.clear();
	index_out = -1;
	if (identity_json.empty())
		return false;

	obs_data_t *identity = obs_data_create_from_json(identity_json.c_str());
	if (!identity)
		return false;

	const char *name = obs_data_get_string(identity, "name");
	if (name)
		name_out = name;
	const char *host_api = obs_data_get_string(identity, "hostapi");
	if (host_api)
		host_api_out = host_api;
	index_out = obs_data_get_int(identity, "index");
	obs_data_release(identity);
	return true;
}

} // namespace

std::vector<MptAudioInputDevice> mpt_audio_backend_enumerate_input_devices()
{
	std::vector<MptAudioInputDevice> devices;

#ifdef _WIN32
	UINT count = waveInGetNumDevs();
	devices.reserve(static_cast<size_t>(count));
	for (UINT idx = 0; idx < count; ++idx) {
		std::string name;
		if (!get_wavein_device_name(idx, name))
			continue;

		MptAudioInputDevice device;
		device.index = idx;
		device.name = std::move(name);
		device.host_api = "MME";
		device.label = std::to_string(idx) + " " + device.name + " [" + device.host_api + "]";
		device.identity_json = build_identity_json(device.index, device.name, device.host_api.c_str());
		devices.push_back(std::move(device));
	}
#else
	std::string error;
	if (!portaudio_acquire(error))
		return devices;

	PaDeviceIndex count = Pa_GetDeviceCount();
	if (count > 0)
		devices.reserve(static_cast<size_t>(count));

	for (PaDeviceIndex idx = 0; idx < count; ++idx) {
		std::string name;
		std::string host_api;
		if (!get_portaudio_device_details(idx, name, host_api))
			continue;

		MptAudioInputDevice device;
		device.index = static_cast<uint32_t>(idx);
		device.name = std::move(name);
		device.host_api = std::move(host_api);
		device.label = std::to_string(device.index) + " " + device.name + " [" + device.host_api + "]";
		device.identity_json = build_identity_json(device.index, device.name, device.host_api.c_str());
		devices.push_back(std::move(device));
	}

	portaudio_release();
#endif

	return devices;
}

bool mpt_audio_backend_resolve_input_device(const std::string &identity_json, long long fallback_index, uint32_t *out_index)
{
	if (out_index)
		*out_index = 0;

	std::string desired_name;
	std::string desired_host_api;
	long long desired_index = -1;
	parse_identity_json(identity_json, desired_name, desired_host_api, desired_index);

#ifdef _WIN32
	UINT count = waveInGetNumDevs();
	if (count == 0)
		return false;

	if (!desired_name.empty()) {
		for (UINT idx = 0; idx < count; ++idx) {
			std::string current_name;
			if (!get_wavein_device_name(idx, current_name))
				continue;
			if (current_name == desired_name) {
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
	std::string error;
	if (!portaudio_acquire(error))
		return false;

	PaDeviceIndex count = Pa_GetDeviceCount();
	if (count <= 0) {
		portaudio_release();
		return false;
	}

	auto matches = [&](PaDeviceIndex idx) -> bool {
		std::string name;
		std::string host_api;
		if (!get_portaudio_device_details(idx, name, host_api))
			return false;
		if (!desired_name.empty() && name != desired_name)
			return false;
		if (!desired_host_api.empty() && host_api != desired_host_api)
			return false;
		return true;
	};

	if (!desired_name.empty()) {
		for (PaDeviceIndex idx = 0; idx < count; ++idx) {
			if (!matches(idx))
				continue;
			if (out_index)
				*out_index = static_cast<uint32_t>(idx);
			portaudio_release();
			return true;
		}
	}

	if (desired_index >= 0 && desired_index < static_cast<long long>(count) && matches(static_cast<PaDeviceIndex>(desired_index))) {
		if (out_index)
			*out_index = static_cast<uint32_t>(desired_index);
		portaudio_release();
		return true;
	}

	if (fallback_index >= 0 && fallback_index < static_cast<long long>(count) && matches(static_cast<PaDeviceIndex>(fallback_index))) {
		if (out_index)
			*out_index = static_cast<uint32_t>(fallback_index);
		portaudio_release();
		return true;
	}

	for (PaDeviceIndex idx = 0; idx < count; ++idx) {
		std::string name;
		std::string host_api;
		if (!get_portaudio_device_details(idx, name, host_api))
			continue;
		if (out_index)
			*out_index = static_cast<uint32_t>(idx);
		portaudio_release();
		return true;
	}

	portaudio_release();
	return false;
#endif
}

bool mpt_audio_backend_start_input_capture(const std::string &identity_json, long long fallback_index, MptAudioInputCallback callback,
					   void *userdata, MptAudioCapture **out_capture, std::string &error)
{
	if (out_capture)
		*out_capture = nullptr;
	if (!out_capture) {
		error = "invalid audio capture output pointer";
		return false;
	}
	if (!callback) {
		error = "invalid audio callback";
		return false;
	}

#ifdef _WIN32
	auto *capture = new (std::nothrow) MptAudioCapture();
	if (!capture) {
		error = "out of memory while creating audio capture";
		return false;
	}

	uint32_t device_index = 0;
	if (!mpt_audio_backend_resolve_input_device(identity_json, fallback_index, &device_index)) {
		delete capture;
		error = "failed to resolve audio input device";
		return false;
	}

	capture->callback = callback;
	capture->userdata = userdata;

	for (const auto &candidate : k_audio_format_candidates) {
		HWAVEIN opened_wave_in = nullptr;
		WAVEFORMATEX wfx = {};
		wfx.wFormatTag = WAVE_FORMAT_PCM;
		wfx.nChannels = candidate.channels;
		wfx.nSamplesPerSec = candidate.sample_rate;
		wfx.wBitsPerSample = 16;
		wfx.nBlockAlign = (WORD)(wfx.nChannels * (wfx.wBitsPerSample / 8));
		wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

		if (waveInOpen(&opened_wave_in, static_cast<UINT>(device_index), &wfx, (DWORD_PTR)&wave_in_callback, (DWORD_PTR)capture,
			       CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
			continue;
		}

		capture->wave_in = opened_wave_in;
		capture->channels = wfx.nChannels;
		capture->sample_rate = wfx.nSamplesPerSec;

		const size_t samples_per_buffer = 1024U;
		bool headers_ready = true;
		for (size_t idx = 0; idx < capture->headers.size(); ++idx) {
			capture->storage[idx].resize(samples_per_buffer * capture->channels);
			memset(&capture->headers[idx], 0, sizeof(WAVEHDR));
			capture->headers[idx].lpData = reinterpret_cast<LPSTR>(capture->storage[idx].data());
			capture->headers[idx].dwBufferLength = static_cast<DWORD>(capture->storage[idx].size() * sizeof(int16_t));
			if (waveInPrepareHeader(opened_wave_in, &capture->headers[idx], sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
			    waveInAddBuffer(opened_wave_in, &capture->headers[idx], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
				headers_ready = false;
				break;
			}
		}

		if (headers_ready && waveInStart(opened_wave_in) == MMSYSERR_NOERROR) {
			*out_capture = capture;
			return true;
		}

		mpt_audio_backend_stop_input_capture(capture);
		capture = new (std::nothrow) MptAudioCapture();
		if (!capture) {
			error = "out of memory while retrying audio capture";
			return false;
		}
		capture->callback = callback;
		capture->userdata = userdata;
	}

	delete capture;
	error = "failed to start WinMM audio capture";
	return false;
#else
	std::string acquire_error;
	if (!portaudio_acquire(acquire_error)) {
		error = std::move(acquire_error);
		return false;
	}

	auto *capture = new (std::nothrow) MptAudioCapture();
	if (!capture) {
		portaudio_release();
		error = "out of memory while creating audio capture";
		return false;
	}

	uint32_t device_index = 0;
	if (!mpt_audio_backend_resolve_input_device(identity_json, fallback_index, &device_index)) {
		delete capture;
		portaudio_release();
		error = "failed to resolve audio input device";
		return false;
	}

	const PaDeviceInfo *device_info = Pa_GetDeviceInfo(static_cast<PaDeviceIndex>(device_index));
	if (!device_info || device_info->maxInputChannels <= 0) {
		delete capture;
		portaudio_release();
		error = "resolved audio device is not a valid input device";
		return false;
	}

	capture->callback = callback;
	capture->userdata = userdata;

	for (const auto &candidate : k_audio_format_candidates) {
		int input_channels = std::min<int>(candidate.channels, device_info->maxInputChannels);
		if (input_channels <= 0)
			continue;

		PaStreamParameters input_parameters {};
		input_parameters.device = static_cast<PaDeviceIndex>(device_index);
		input_parameters.channelCount = input_channels;
		input_parameters.sampleFormat = paInt16;
		input_parameters.suggestedLatency =
			device_info->defaultLowInputLatency > 0.0 ? device_info->defaultLowInputLatency : device_info->defaultHighInputLatency;
		input_parameters.hostApiSpecificStreamInfo = nullptr;

		PaError err = Pa_IsFormatSupported(&input_parameters, nullptr, static_cast<double>(candidate.sample_rate));
		if (err != paFormatIsSupported)
			continue;

		err = Pa_OpenStream(&capture->stream, &input_parameters, nullptr, static_cast<double>(candidate.sample_rate), 256,
				    paNoFlag, &portaudio_input_callback, capture);
		if (err != paNoError) {
			capture->stream = nullptr;
			continue;
		}

		capture->channels = static_cast<uint16_t>(input_channels);
		capture->sample_rate = candidate.sample_rate;
		err = Pa_StartStream(capture->stream);
		if (err == paNoError) {
			*out_capture = capture;
			return true;
		}

		Pa_CloseStream(capture->stream);
		capture->stream = nullptr;
	}

	delete capture;
	portaudio_release();
	error = "failed to start PortAudio input capture";
	return false;
#endif
}

void mpt_audio_backend_stop_input_capture(MptAudioCapture *capture)
{
	if (!capture)
		return;

#ifdef _WIN32
	if (capture->wave_in) {
		waveInStop(capture->wave_in);
		waveInReset(capture->wave_in);
		for (auto &header : capture->headers) {
			if (header.dwFlags & WHDR_PREPARED)
				waveInUnprepareHeader(capture->wave_in, &header, sizeof(WAVEHDR));
		}
		waveInClose(capture->wave_in);
		capture->wave_in = nullptr;
	}
#else
	if (capture->stream) {
		Pa_StopStream(capture->stream);
		Pa_CloseStream(capture->stream);
		capture->stream = nullptr;
	}
	portaudio_release();
#endif

	delete capture;
}
