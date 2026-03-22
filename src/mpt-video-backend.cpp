#include "mpt-video-backend.h"

#include <cstring>
#include <new>

#ifdef _WIN32
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <propidl.h>
#endif

struct MptVideoBackend {
#ifdef _WIN32
	IMFSourceReader *reader = nullptr;
	bool mf_started = false;
#endif
};

namespace {

#ifdef _WIN32

template<typename T>
void safe_release(T **ptr)
{
	if (ptr && *ptr) {
		(*ptr)->Release();
		*ptr = nullptr;
	}
}

static std::wstring utf8_to_wide(const char *text)
{
	if (!text || !*text)
		return std::wstring();

	int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
	if (needed <= 0)
		return std::wstring();

	std::wstring out(static_cast<size_t>(needed), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), needed);
	if (!out.empty() && out.back() == L'\0')
		out.pop_back();
	return out;
}

#endif

} // namespace

bool mpt_video_backend_create(MptVideoBackend **out_backend, std::string &error)
{
	if (out_backend)
		*out_backend = nullptr;
	if (!out_backend) {
		error = "invalid video backend output pointer";
		return false;
	}

#ifdef _WIN32
	auto *backend = new (std::nothrow) MptVideoBackend();
	if (!backend) {
		error = "out of memory while creating video backend";
		return false;
	}

	HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
	if (FAILED(hr)) {
		delete backend;
		error = "MFStartup failed";
		return false;
	}

	backend->mf_started = true;
	*out_backend = backend;
	return true;
#else
	error = "video backend is only implemented for Windows builds";
	return false;
#endif
}

void mpt_video_backend_destroy(MptVideoBackend *backend)
{
	if (!backend)
		return;

#ifdef _WIN32
	safe_release(&backend->reader);
	if (backend->mf_started)
		MFShutdown();
#endif

	delete backend;
}

bool mpt_video_backend_open_loop_video(MptVideoBackend *backend, const std::string &loop_video_path, ImageBGRA *out_frame,
				       std::string &error)
{
	if (!out_frame) {
		error = "invalid video output frame";
		return false;
	}

#ifdef _WIN32
	if (!backend) {
		error = "video backend is not initialized";
		return false;
	}
	if (loop_video_path.empty()) {
		error = "Loop video is required.";
		return false;
	}

	IMFAttributes *attributes = nullptr;
	HRESULT hr = MFCreateAttributes(&attributes, 1);
	if (FAILED(hr)) {
		error = "failed to create Media Foundation attributes";
		return false;
	}

	attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
	std::wstring video_path = utf8_to_wide(loop_video_path.c_str());
	hr = MFCreateSourceReaderFromURL(video_path.c_str(), attributes, &backend->reader);
	safe_release(&attributes);
	if (FAILED(hr) || !backend->reader) {
		error = "failed to open loop video with Media Foundation";
		return false;
	}

	backend->reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
	backend->reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

	IMFMediaType *type = nullptr;
	hr = MFCreateMediaType(&type);
	if (FAILED(hr)) {
		error = "failed to create Media Foundation media type";
		return false;
	}

	type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
	hr = backend->reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
	safe_release(&type);
	if (FAILED(hr)) {
		error = "failed to configure RGB32 video output";
		return false;
	}

	IMFMediaType *current_type = nullptr;
	hr = backend->reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current_type);
	if (FAILED(hr) || !current_type) {
		error = "failed to read video media type";
		return false;
	}

	UINT32 width = 0;
	UINT32 height = 0;
	MFGetAttributeSize(current_type, MF_MT_FRAME_SIZE, &width, &height);
	safe_release(&current_type);
	if (width == 0 || height == 0) {
		error = "video dimensions are invalid";
		return false;
	}

	out_frame->width = width;
	out_frame->height = height;
	out_frame->pixels.resize(static_cast<size_t>(width) * height * 4U);
	return true;
#else
	(void)backend;
	(void)loop_video_path;
	error = "video backend is only implemented for Windows builds";
	return false;
#endif
}

bool mpt_video_backend_read_next_frame(MptVideoBackend *backend, ImageBGRA &image, uint64_t &timestamp_ns)
{
	timestamp_ns = 0;

#ifdef _WIN32
	if (!backend || !backend->reader)
		return false;

	for (;;) {
		DWORD flags = 0;
		LONGLONG timestamp = 0;
		IMFSample *sample = nullptr;
		HRESULT hr =
			backend->reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &timestamp, &sample);
		if (FAILED(hr))
			return false;

		if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
			PROPVARIANT position;
			PropVariantInit(&position);
			position.vt = VT_I8;
			position.hVal.QuadPart = 0;
			backend->reader->SetCurrentPosition(GUID_NULL, position);
			PropVariantClear(&position);
			continue;
		}

		if (!sample)
			continue;

		IMFMediaBuffer *buffer = nullptr;
		hr = sample->ConvertToContiguousBuffer(&buffer);
		safe_release(&sample);
		if (FAILED(hr) || !buffer) {
			safe_release(&buffer);
			return false;
		}

		BYTE *data = nullptr;
		DWORD max_length = 0;
		DWORD current_length = 0;
		hr = buffer->Lock(&data, &max_length, &current_length);
		if (FAILED(hr) || !data) {
			safe_release(&buffer);
			return false;
		}

		size_t expected = image.pixels.size();
		if (current_length < expected) {
			buffer->Unlock();
			safe_release(&buffer);
			return false;
		}

		memcpy(image.pixels.data(), data, expected);
		for (size_t idx = 3; idx < image.pixels.size(); idx += 4)
			image.pixels[idx] = 255;

		buffer->Unlock();
		safe_release(&buffer);
		timestamp_ns = static_cast<uint64_t>(timestamp) * 100ULL;
		return true;
	}
#else
	(void)backend;
	(void)image;
	return false;
#endif
}
