#include "mpt-audio-backend.h"

#include "motionpngtuber-native.h"
#include "plugin-support.h"

#include <atomic>
#include <array>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <mmeapi.h>

struct MptAudioCapture {
	HWAVEIN wave_in = nullptr;
	uint16_t channels = 1;
	uint32_t sample_rate = 44100;
	std::atomic<MptAudioInputCallback> callback {nullptr};
	std::atomic<void *> userdata {nullptr};
	std::array<std::vector<int16_t>, 3> storage {};
	std::array<WAVEHDR, 3> headers {};
	std::atomic<uint32_t> active_callbacks {0};
	std::atomic<bool> stop_requested {false};
	HANDLE data_ready_event = nullptr;
	HANDLE stop_event = nullptr;
	HANDLE close_event = nullptr;
	HANDLE worker_thread = nullptr;

	~MptAudioCapture()
	{
		if (worker_thread)
			CloseHandle(worker_thread);
		if (data_ready_event)
			CloseHandle(data_ready_event);
		if (stop_event)
			CloseHandle(stop_event);
		if (close_event)
			CloseHandle(close_event);
	}
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

static void log_wavein_warning(const char *operation, MMRESULT result)
{
	char text[MAXERRORLENGTH] = {};
	if (waveInGetErrorTextA(result, text, MAXERRORLENGTH) != MMSYSERR_NOERROR || !text[0]) {
		obs_log(LOG_WARNING, "%s failed for MotionPngTuberPlayer audio capture (MMRESULT=%u)", operation,
			(unsigned int)result);
		return;
	}

	obs_log(LOG_WARNING, "%s failed for MotionPngTuberPlayer audio capture: %s (MMRESULT=%u)", operation, text,
		(unsigned int)result);
}

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
	if (!capture || !samples || sample_count == 0)
		return;

	MptAudioInputCallback callback = capture->callback.load(std::memory_order_acquire);
	void *userdata = capture->userdata.load(std::memory_order_acquire);
	if (!callback)
		return;
	callback(samples, sample_count, capture->channels, capture->sample_rate, userdata);
}

static void process_pending_audio_buffers(MptAudioCapture *capture)
{
	if (!capture)
		return;

	for (auto &header : capture->headers) {
		if ((header.dwFlags & WHDR_DONE) == 0)
			continue;

		size_t sample_count = header.dwBytesRecorded / sizeof(int16_t);
		if (!capture->stop_requested.load(std::memory_order_acquire) && sample_count > 0)
			dispatch_audio_samples(capture, reinterpret_cast<const int16_t *>(header.lpData), sample_count);

		header.dwBytesRecorded = 0;
		if (capture->stop_requested.load(std::memory_order_acquire))
			continue;

		if (!capture->wave_in)
			continue;

		MMRESULT result = waveInAddBuffer(capture->wave_in, &header, sizeof(WAVEHDR));
		if (result != MMSYSERR_NOERROR)
			log_wavein_warning("waveInAddBuffer", result);
	}
}

static DWORD WINAPI audio_capture_worker_thread(void *param)
{
	auto *capture = reinterpret_cast<MptAudioCapture *>(param);
	if (!capture || !capture->stop_event || !capture->data_ready_event)
		return 0;

	HANDLE handles[] = {capture->stop_event, capture->data_ready_event};
	for (;;) {
		DWORD wait_result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
		if (wait_result == WAIT_OBJECT_0)
			return 0;
		if (wait_result != WAIT_OBJECT_0 + 1) {
			obs_log(LOG_WARNING, "MotionPngTuberPlayer audio worker wait failed during capture");
			return 0;
		}

		process_pending_audio_buffers(capture);
	}
}

static MptAudioCapture *create_audio_capture(MptAudioInputCallback callback, void *userdata)
{
	auto *capture = new (std::nothrow) MptAudioCapture();
	if (!capture)
		return nullptr;

	capture->callback.store(callback, std::memory_order_release);
	capture->userdata.store(userdata, std::memory_order_release);
	capture->data_ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	capture->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	capture->close_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!capture->data_ready_event || !capture->stop_event || !capture->close_event) {
		delete capture;
		return nullptr;
	}

	return capture;
}

static void CALLBACK wave_in_callback(HWAVEIN hwi, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2)
{
	UNUSED_PARAMETER(hwi);
	UNUSED_PARAMETER(param1);
	UNUSED_PARAMETER(param2);
	if (!instance)
		return;

	auto *capture = reinterpret_cast<MptAudioCapture *>(instance);
	if (!capture)
		return;

	capture->active_callbacks.fetch_add(1, std::memory_order_acq_rel);
	if (msg == WIM_DATA) {
		if (capture->data_ready_event)
			SetEvent(capture->data_ready_event);
		capture->active_callbacks.fetch_sub(1, std::memory_order_acq_rel);
		return;
	}

	if (msg == WIM_CLOSE && capture->close_event)
		SetEvent(capture->close_event);
	capture->active_callbacks.fetch_sub(1, std::memory_order_acq_rel);
}

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

	auto *capture = create_audio_capture(callback, userdata);
	if (!capture) {
		error = "failed to allocate audio capture state";
		return false;
	}

	uint32_t device_index = 0;
	if (!mpt_audio_backend_resolve_input_device(identity_json, fallback_index, &device_index)) {
		delete capture;
		error = "failed to resolve audio input device";
		return false;
	}

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

		if (headers_ready) {
			capture->worker_thread = CreateThread(nullptr, 0, &audio_capture_worker_thread, capture, 0, nullptr);
			if (!capture->worker_thread) {
				error = "failed to start audio capture worker thread";
				headers_ready = false;
			}
		}

		if (headers_ready && waveInStart(opened_wave_in) == MMSYSERR_NOERROR) {
			*out_capture = capture;
			return true;
		}

		mpt_audio_backend_stop_input_capture(capture);
		capture = create_audio_capture(callback, userdata);
		if (!capture) {
			error = "failed to allocate audio capture state";
			return false;
		}
	}

	delete capture;
	error = "failed to start WinMM audio capture";
	return false;
}

void mpt_audio_backend_stop_input_capture(MptAudioCapture *capture)
{
	if (!capture)
		return;

	capture->callback.store(nullptr, std::memory_order_release);
	capture->userdata.store(nullptr, std::memory_order_release);
	capture->stop_requested.store(true, std::memory_order_release);
	if (capture->stop_event)
		SetEvent(capture->stop_event);
	if (capture->worker_thread) {
		WaitForSingleObject(capture->worker_thread, INFINITE);
		CloseHandle(capture->worker_thread);
		capture->worker_thread = nullptr;
	}

	if (capture->wave_in) {
		MMRESULT stop_result = waveInStop(capture->wave_in);
		if (stop_result != MMSYSERR_NOERROR)
			log_wavein_warning("waveInStop", stop_result);
		MMRESULT reset_result = waveInReset(capture->wave_in);
		if (reset_result != MMSYSERR_NOERROR)
			log_wavein_warning("waveInReset", reset_result);
		for (auto &header : capture->headers) {
			if ((header.dwFlags & WHDR_PREPARED) == 0)
				continue;
			MMRESULT unprepare_result = waveInUnprepareHeader(capture->wave_in, &header, sizeof(WAVEHDR));
			if (unprepare_result != MMSYSERR_NOERROR)
				log_wavein_warning("waveInUnprepareHeader", unprepare_result);
		}
		if (capture->close_event)
			ResetEvent(capture->close_event);
		MMRESULT close_result = waveInClose(capture->wave_in);
		if (close_result != MMSYSERR_NOERROR) {
			log_wavein_warning("waveInClose", close_result);
		} else if (capture->close_event) {
			WaitForSingleObject(capture->close_event, INFINITE);
			while (capture->active_callbacks.load(std::memory_order_acquire) != 0)
				Sleep(1);
		}
		capture->wave_in = nullptr;
	}

	delete capture;
}
