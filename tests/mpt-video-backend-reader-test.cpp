#include "mpt-video-backend.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <propidl.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

struct MptVideoBackend {
	IMFSourceReader *reader = nullptr;
	bool mf_started = false;
};

namespace {

struct ReaderStep {
	HRESULT read_hr = S_OK;
	DWORD flags = 0;
	LONGLONG timestamp = 0;
	IMFSample *sample = nullptr;
};

class FakeSourceReader : public IMFSourceReader {
public:
	FakeSourceReader(std::vector<ReaderStep> steps, HRESULT set_current_position_hr)
		: steps_(std::move(steps)), set_current_position_hr_(set_current_position_hr)
	{
	}

	~FakeSourceReader()
	{
		for (ReaderStep &step : steps_) {
			if (step.sample)
				step.sample->Release();
		}
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++ref_count_;
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG ref_count = --ref_count_;
		if (ref_count == 0)
			delete this;
		return ref_count;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
	{
		if (!ppvObject)
			return E_POINTER;
		*ppvObject = nullptr;
		if (riid == IID_IUnknown || riid == IID_IMFSourceReader) {
			*ppvObject = static_cast<IMFSourceReader *>(this);
			AddRef();
			return S_OK;
		}
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE GetStreamSelection(DWORD, BOOL *pfSelected) override
	{
		if (!pfSelected)
			return E_POINTER;
		*pfSelected = TRUE;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetStreamSelection(DWORD, BOOL) override
	{
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetNativeMediaType(DWORD, DWORD, IMFMediaType **) override
	{
		return E_NOTIMPL;
	}

	HRESULT STDMETHODCALLTYPE GetCurrentMediaType(DWORD, IMFMediaType **) override
	{
		return E_NOTIMPL;
	}

	HRESULT STDMETHODCALLTYPE SetCurrentMediaType(DWORD, DWORD *, IMFMediaType *) override
	{
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetCurrentPosition(REFGUID, REFPROPVARIANT) override
	{
		++set_current_position_calls_;
		return set_current_position_hr_;
	}

	HRESULT STDMETHODCALLTYPE ReadSample(DWORD, DWORD, DWORD *pdwActualStreamIndex, DWORD *pdwStreamFlags,
					     LONGLONG *pllTimestamp, IMFSample **ppSample) override
	{
		++read_sample_calls_;
		if (pdwActualStreamIndex)
			*pdwActualStreamIndex = MF_SOURCE_READER_FIRST_VIDEO_STREAM;
		if (pdwStreamFlags)
			*pdwStreamFlags = 0;
		if (pllTimestamp)
			*pllTimestamp = 0;
		if (ppSample)
			*ppSample = nullptr;

		if (next_step_ >= steps_.size())
			return E_FAIL;

		const ReaderStep &step = steps_[next_step_++];
		if (pdwStreamFlags)
			*pdwStreamFlags = step.flags;
		if (pllTimestamp)
			*pllTimestamp = step.timestamp;
		if (ppSample && step.sample) {
			step.sample->AddRef();
			*ppSample = step.sample;
		}
		return step.read_hr;
	}

	HRESULT STDMETHODCALLTYPE Flush(DWORD) override
	{
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetServiceForStream(DWORD, REFGUID, REFIID, LPVOID *) override
	{
		return E_NOTIMPL;
	}

	HRESULT STDMETHODCALLTYPE GetPresentationAttribute(DWORD, REFGUID, PROPVARIANT *) override
	{
		return E_NOTIMPL;
	}

	ULONG read_sample_calls() const
	{
		return read_sample_calls_;
	}

	ULONG set_current_position_calls() const
	{
		return set_current_position_calls_;
	}

private:
	std::atomic<ULONG> ref_count_ {1};
	std::vector<ReaderStep> steps_;
	size_t next_step_ = 0;
	HRESULT set_current_position_hr_ = S_OK;
	ULONG read_sample_calls_ = 0;
	ULONG set_current_position_calls_ = 0;
};

IMFSample *make_sample(const std::vector<uint8_t> &bytes)
{
	IMFSample *sample = nullptr;
	IMFMediaBuffer *buffer = nullptr;
	BYTE *data = nullptr;
	DWORD max_length = 0;
	DWORD current_length = 0;

	HRESULT hr = MFCreateSample(&sample);
	if (FAILED(hr) || !sample)
		return nullptr;

	hr = MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), &buffer);
	if (FAILED(hr) || !buffer) {
		if (sample)
			sample->Release();
		return nullptr;
	}

	hr = buffer->Lock(&data, &max_length, &current_length);
	if (FAILED(hr) || !data) {
		buffer->Release();
		sample->Release();
		return nullptr;
	}

	std::memcpy(data, bytes.data(), bytes.size());
	buffer->Unlock();
	buffer->SetCurrentLength(static_cast<DWORD>(bytes.size()));
	hr = sample->AddBuffer(buffer);
	buffer->Release();
	if (FAILED(hr)) {
		sample->Release();
		return nullptr;
	}

	return sample;
}

ImageBGRA make_image()
{
	ImageBGRA image;
	image.width = 2;
	image.height = 1;
	image.pixels.resize(8);
	return image;
}

bool create_backend(MptVideoBackend **out_backend)
{
	std::string error;
	if (!mpt_video_backend_create(out_backend, error) || !out_backend || !*out_backend) {
		std::cerr << "mpt_video_backend_create failed: " << error << '\n';
		return false;
	}
	return true;
}

bool expect(bool condition, const char *message)
{
	if (!condition)
		std::cerr << message << '\n';
	return condition;
}

bool test_end_of_stream_seek_success_returns_next_frame()
{
	MptVideoBackend *backend = nullptr;
	if (!create_backend(&backend))
		return false;

	const std::vector<uint8_t> raw_bytes = {1, 2, 3, 4, 5, 6, 7, 8};
	IMFSample *sample = make_sample(raw_bytes);
	if (!sample) {
		std::cerr << "failed to create Media Foundation sample\n";
		mpt_video_backend_destroy(backend);
		return false;
	}

	auto *reader = new FakeSourceReader(
		{
			{S_OK, MF_SOURCE_READERF_ENDOFSTREAM, 0, nullptr},
			{S_OK, 0, 321, sample},
		},
		S_OK);
	backend->reader = reader;

	ImageBGRA image = make_image();
	uint64_t timestamp_ns = 0;
	const bool ok = mpt_video_backend_read_next_frame(backend, image, timestamp_ns);
	mpt_video_backend_destroy(backend);

	return expect(ok, "end-of-stream with successful seek should keep reading") &&
	       expect(timestamp_ns == 32100ULL, "timestamp should be converted to 100ns units") &&
	       expect(image.pixels.size() == 8, "expected image buffer size") &&
	       expect(image.pixels[0] == 1 && image.pixels[1] == 2 && image.pixels[2] == 3 && image.pixels[3] == 255 &&
			      image.pixels[4] == 5 && image.pixels[5] == 6 && image.pixels[6] == 7 && image.pixels[7] == 255,
		      "sample bytes should be copied and alpha forced opaque");
}

bool test_reader_error_flag_aborts_read()
{
	MptVideoBackend *backend = nullptr;
	if (!create_backend(&backend))
		return false;

	IMFSample *sample = make_sample({1, 2, 3, 4, 5, 6, 7, 8});
	if (!sample) {
		std::cerr << "failed to create Media Foundation sample\n";
		mpt_video_backend_destroy(backend);
		return false;
	}

	auto *reader = new FakeSourceReader(
		{
			{S_OK, MF_SOURCE_READERF_ERROR, 0, nullptr},
			{S_OK, 0, 100, sample},
		},
		S_OK);
	backend->reader = reader;

	ImageBGRA image = make_image();
	uint64_t timestamp_ns = 12345;
	const bool ok = mpt_video_backend_read_next_frame(backend, image, timestamp_ns);
	mpt_video_backend_destroy(backend);

	return expect(!ok, "reader error flag should abort frame reads instead of skipping to a later sample");
}

bool test_failed_loop_seek_aborts_read()
{
	MptVideoBackend *backend = nullptr;
	if (!create_backend(&backend))
		return false;

	IMFSample *sample = make_sample({1, 2, 3, 4, 5, 6, 7, 8});
	if (!sample) {
		std::cerr << "failed to create Media Foundation sample\n";
		mpt_video_backend_destroy(backend);
		return false;
	}

	auto *reader = new FakeSourceReader(
		{
			{S_OK, MF_SOURCE_READERF_ENDOFSTREAM, 0, nullptr},
			{S_OK, 0, 100, sample},
		},
		E_FAIL);
	backend->reader = reader;

	ImageBGRA image = make_image();
	uint64_t timestamp_ns = 0;
	const bool ok = mpt_video_backend_read_next_frame(backend, image, timestamp_ns);
	const ULONG set_position_calls = reader->set_current_position_calls();
	mpt_video_backend_destroy(backend);

	return expect(!ok, "loop seek failure should abort frame reads instead of continuing with stale reader state") &&
	       expect(set_position_calls == 1, "loop seek should be attempted once when end-of-stream is reached");
}

bool test_too_many_null_samples_abort_read()
{
	MptVideoBackend *backend = nullptr;
	if (!create_backend(&backend))
		return false;

	std::vector<ReaderStep> steps;
	steps.reserve(129);
	for (size_t idx = 0; idx < 128; ++idx)
		steps.push_back({S_OK, 0, static_cast<LONGLONG>(idx), nullptr});
	steps.push_back({S_OK, 0, 999, make_sample({1, 2, 3, 4, 5, 6, 7, 8})});
	if (!steps.back().sample) {
		std::cerr << "failed to create Media Foundation sample\n";
		mpt_video_backend_destroy(backend);
		return false;
	}

	auto *reader = new FakeSourceReader(std::move(steps), S_OK);
	backend->reader = reader;

	ImageBGRA image = make_image();
	uint64_t timestamp_ns = 0;
	const bool ok = mpt_video_backend_read_next_frame(backend, image, timestamp_ns);
	const ULONG read_calls = reader->read_sample_calls();
	mpt_video_backend_destroy(backend);

	return expect(!ok, "too many null samples should abort frame reads instead of spinning indefinitely") &&
	       expect(read_calls < 129, "null sample handling should stop before exhausting all queued retries");
}

} // namespace

int main()
{
	struct TestCase {
		const char *name;
		bool (*fn)();
	};

	const TestCase tests[] = {
		{"end-of-stream seek success returns next frame", test_end_of_stream_seek_success_returns_next_frame},
		{"reader error flag aborts read", test_reader_error_flag_aborts_read},
		{"failed loop seek aborts read", test_failed_loop_seek_aborts_read},
		{"too many null samples abort read", test_too_many_null_samples_abort_read},
	};

	int failures = 0;
	for (const TestCase &test : tests) {
		const bool passed = test.fn();
		std::cout << (passed ? "[PASS] " : "[FAIL] ") << test.name << '\n';
		if (!passed)
			++failures;
	}

	return failures == 0 ? 0 : 1;
}
