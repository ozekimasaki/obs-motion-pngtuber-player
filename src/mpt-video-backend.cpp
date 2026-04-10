#include "mpt-video-backend.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>

#if defined(_WIN32)

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <propidl.h>

struct MptVideoBackend {
	IMFSourceReader *reader = nullptr;
	bool mf_started = false;
};

namespace {

constexpr size_t MAX_EMPTY_SOURCE_READS = 64;
constexpr size_t MAX_END_OF_STREAM_REWINDS = 8;

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

static bool seek_reader_to_start(IMFSourceReader *reader)
{
	if (!reader)
		return false;

	PROPVARIANT position;
	PropVariantInit(&position);
	position.vt = VT_I8;
	position.hVal.QuadPart = 0;
	const HRESULT hr = reader->SetCurrentPosition(GUID_NULL, position);
	PropVariantClear(&position);
	return SUCCEEDED(hr);
}

} // namespace

bool mpt_video_backend_create(MptVideoBackend **out_backend, std::string &error)
{
	if (out_backend)
		*out_backend = nullptr;
	if (!out_backend) {
		error = "invalid video backend output pointer";
		return false;
	}

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
}

void mpt_video_backend_destroy(MptVideoBackend *backend)
{
	if (!backend)
		return;

	safe_release(&backend->reader);
	if (backend->mf_started)
		MFShutdown();
	delete backend;
}

bool mpt_video_backend_open_loop_video(MptVideoBackend *backend, const std::string &loop_video_path, ImageBGRA *out_frame,
				       std::string &error)
{
	if (!out_frame) {
		error = "invalid video output frame";
		return false;
	}
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
}

bool mpt_video_backend_read_next_frame(MptVideoBackend *backend, ImageBGRA &image, uint64_t &timestamp_ns)
{
	timestamp_ns = 0;
	if (!backend || !backend->reader)
		return false;

	size_t empty_reads = 0;
	size_t end_of_stream_rewinds = 0;
	for (;;) {
		DWORD flags = 0;
		LONGLONG timestamp = 0;
		IMFSample *sample = nullptr;
		HRESULT hr =
			backend->reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &timestamp, &sample);
		if (FAILED(hr))
			return false;

		if (flags & MF_SOURCE_READERF_ERROR) {
			safe_release(&sample);
			return false;
		}

		if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
			safe_release(&sample);
			if (++end_of_stream_rewinds > MAX_END_OF_STREAM_REWINDS)
				return false;
			if (!seek_reader_to_start(backend->reader))
				return false;
			continue;
		}

		if ((flags & MF_SOURCE_READERF_STREAMTICK) || !sample) {
			safe_release(&sample);
			if (++empty_reads > MAX_EMPTY_SOURCE_READS)
				return false;
			continue;
		}

		empty_reads = 0;
		end_of_stream_rewinds = 0;

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
}

#else

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

struct MptVideoBackend {
	AVFormatContext *format_context = nullptr;
	AVCodecContext *codec_context = nullptr;
	SwsContext *sws_context = nullptr;
	AVFrame *decoded_frame = nullptr;
	AVPacket *packet = nullptr;
	int video_stream_index = -1;
	uint64_t frame_duration_ns = 33333333ULL;
	uint64_t fallback_timestamp_ns = 0;
	bool end_of_stream_sent = false;
	bool packet_pending = false;
};

namespace {

constexpr size_t MAX_EMPTY_SOURCE_READS = 64;
constexpr size_t MAX_END_OF_STREAM_REWINDS = 8;

static std::string ffmpeg_error_string(int error_code)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(error_code, buffer, sizeof(buffer));
	return std::string(buffer);
}

static void reset_backend_state(MptVideoBackend *backend)
{
	if (!backend)
		return;
	if (backend->packet)
		av_packet_free(&backend->packet);
	if (backend->decoded_frame)
		av_frame_free(&backend->decoded_frame);
	if (backend->sws_context) {
		sws_freeContext(backend->sws_context);
		backend->sws_context = nullptr;
	}
	if (backend->codec_context)
		avcodec_free_context(&backend->codec_context);
	if (backend->format_context)
		avformat_close_input(&backend->format_context);
	backend->video_stream_index = -1;
	backend->frame_duration_ns = 33333333ULL;
	backend->fallback_timestamp_ns = 0;
	backend->end_of_stream_sent = false;
	backend->packet_pending = false;
}

static bool seek_reader_to_start(MptVideoBackend *backend)
{
	if (!backend || !backend->format_context || !backend->codec_context)
		return false;

	int result = av_seek_frame(backend->format_context, backend->video_stream_index, 0, AVSEEK_FLAG_BACKWARD);
	if (result < 0)
		result = avformat_seek_file(backend->format_context, backend->video_stream_index, INT64_MIN, 0, INT64_MAX, 0);
	if (result < 0)
		return false;

	avcodec_flush_buffers(backend->codec_context);
	backend->end_of_stream_sent = false;
	backend->fallback_timestamp_ns = 0;
	backend->packet_pending = false;
	return true;
}

static uint64_t decode_frame_timestamp_ns(MptVideoBackend *backend)
{
	if (!backend || !backend->decoded_frame || !backend->format_context)
		return 0;

	AVStream *stream = backend->format_context->streams[backend->video_stream_index];
	int64_t pts = backend->decoded_frame->best_effort_timestamp;
	if (pts == AV_NOPTS_VALUE)
		pts = backend->decoded_frame->pts;

	if (pts != AV_NOPTS_VALUE) {
		uint64_t timestamp_ns = static_cast<uint64_t>(std::max<int64_t>(
			0, av_rescale_q(pts, stream->time_base, AVRational{1, 1000000000})));
		backend->fallback_timestamp_ns = timestamp_ns + backend->frame_duration_ns;
		return timestamp_ns;
	}

	uint64_t timestamp_ns = backend->fallback_timestamp_ns;
	backend->fallback_timestamp_ns += backend->frame_duration_ns;
	return timestamp_ns;
}

static bool receive_decoded_frame(MptVideoBackend *backend, ImageBGRA &image, uint64_t &timestamp_ns)
{
	if (!backend || !backend->codec_context || !backend->decoded_frame || !backend->sws_context)
		return false;

	int result = avcodec_receive_frame(backend->codec_context, backend->decoded_frame);
	if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
		return false;
	if (result < 0)
		return false;

	uint8_t *dst_data[4] = {image.pixels.data(), nullptr, nullptr, nullptr};
	int dst_linesize[4] = {(int)image.stride(), 0, 0, 0};
	sws_scale(backend->sws_context, backend->decoded_frame->data, backend->decoded_frame->linesize, 0,
		  backend->codec_context->height, dst_data, dst_linesize);
	for (size_t idx = 3; idx < image.pixels.size(); idx += 4)
		image.pixels[idx] = 255;

	timestamp_ns = decode_frame_timestamp_ns(backend);
	av_frame_unref(backend->decoded_frame);
	return true;
}

} // namespace

bool mpt_video_backend_create(MptVideoBackend **out_backend, std::string &error)
{
	if (out_backend)
		*out_backend = nullptr;
	if (!out_backend) {
		error = "invalid video backend output pointer";
		return false;
	}

	auto *backend = new (std::nothrow) MptVideoBackend();
	if (!backend) {
		error = "out of memory while creating video backend";
		return false;
	}

	*out_backend = backend;
	return true;
}

void mpt_video_backend_destroy(MptVideoBackend *backend)
{
	if (!backend)
		return;

	reset_backend_state(backend);
	delete backend;
}

bool mpt_video_backend_open_loop_video(MptVideoBackend *backend, const std::string &loop_video_path, ImageBGRA *out_frame,
				       std::string &error)
{
	if (!out_frame) {
		error = "invalid video output frame";
		return false;
	}
	if (!backend) {
		error = "video backend is not initialized";
		return false;
	}
	if (loop_video_path.empty()) {
		error = "Loop video is required.";
		return false;
	}

	reset_backend_state(backend);

	int result = avformat_open_input(&backend->format_context, loop_video_path.c_str(), nullptr, nullptr);
	if (result < 0) {
		error = "failed to open loop video with FFmpeg: " + ffmpeg_error_string(result);
		reset_backend_state(backend);
		return false;
	}

	result = avformat_find_stream_info(backend->format_context, nullptr);
	if (result < 0) {
		error = "failed to inspect loop video streams: " + ffmpeg_error_string(result);
		reset_backend_state(backend);
		return false;
	}

	result = av_find_best_stream(backend->format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (result < 0) {
		error = "failed to find a video stream in the loop video";
		reset_backend_state(backend);
		return false;
	}
	backend->video_stream_index = result;

	AVStream *stream = backend->format_context->streams[backend->video_stream_index];
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec) {
		error = "failed to find a decoder for the loop video";
		reset_backend_state(backend);
		return false;
	}

	backend->codec_context = avcodec_alloc_context3(codec);
	if (!backend->codec_context) {
		error = "failed to allocate the FFmpeg video decoder";
		reset_backend_state(backend);
		return false;
	}

	result = avcodec_parameters_to_context(backend->codec_context, stream->codecpar);
	if (result < 0) {
		error = "failed to copy loop video codec parameters: " + ffmpeg_error_string(result);
		reset_backend_state(backend);
		return false;
	}

	result = avcodec_open2(backend->codec_context, codec, nullptr);
	if (result < 0) {
		error = "failed to open the loop video decoder: " + ffmpeg_error_string(result);
		reset_backend_state(backend);
		return false;
	}

	backend->decoded_frame = av_frame_alloc();
	backend->packet = av_packet_alloc();
	if (!backend->decoded_frame || !backend->packet) {
		error = "failed to allocate FFmpeg frame buffers";
		reset_backend_state(backend);
		return false;
	}

	backend->sws_context =
		sws_getContext(backend->codec_context->width, backend->codec_context->height, backend->codec_context->pix_fmt,
			       backend->codec_context->width, backend->codec_context->height, AV_PIX_FMT_BGRA, SWS_BILINEAR,
			       nullptr, nullptr, nullptr);
	if (!backend->sws_context) {
		error = "failed to create the loop video color converter";
		reset_backend_state(backend);
		return false;
	}

	AVRational frame_rate = av_guess_frame_rate(backend->format_context, stream, nullptr);
	if (frame_rate.num > 0 && frame_rate.den > 0) {
		uint64_t guessed_frame_duration =
			static_cast<uint64_t>(std::max<int64_t>(1, av_rescale_q(1, av_inv_q(frame_rate), AVRational{1, 1000000000})));
		backend->frame_duration_ns = guessed_frame_duration;
	}

	out_frame->width = (uint32_t)backend->codec_context->width;
	out_frame->height = (uint32_t)backend->codec_context->height;
	out_frame->pixels.assign((size_t)out_frame->width * out_frame->height * 4U, 0);
	return true;
}

bool mpt_video_backend_read_next_frame(MptVideoBackend *backend, ImageBGRA &image, uint64_t &timestamp_ns)
{
	timestamp_ns = 0;
	if (!backend || !backend->format_context || !backend->codec_context || !backend->decoded_frame || !backend->packet)
		return false;

	size_t empty_reads = 0;
	size_t end_of_stream_rewinds = 0;

	for (;;) {
		if (receive_decoded_frame(backend, image, timestamp_ns))
			return true;

		if (backend->packet_pending) {
			int result = avcodec_send_packet(backend->codec_context, backend->packet);
			if (result == AVERROR(EAGAIN))
				continue;
			av_packet_unref(backend->packet);
			backend->packet_pending = false;
			if (result < 0)
				return false;
			empty_reads = 0;
			end_of_stream_rewinds = 0;
			continue;
		}

		if (backend->end_of_stream_sent) {
			if (++end_of_stream_rewinds > MAX_END_OF_STREAM_REWINDS)
				return false;
			if (!seek_reader_to_start(backend))
				return false;
			continue;
		}

		int result = av_read_frame(backend->format_context, backend->packet);
		if (result == AVERROR_EOF) {
			backend->end_of_stream_sent = true;
			result = avcodec_send_packet(backend->codec_context, nullptr);
			if (result < 0 && result != AVERROR_EOF)
				return false;
			continue;
		}
		if (result < 0)
			return false;

		if (backend->packet->stream_index != backend->video_stream_index) {
			av_packet_unref(backend->packet);
			if (++empty_reads > MAX_EMPTY_SOURCE_READS)
				return false;
			continue;
		}

		result = avcodec_send_packet(backend->codec_context, backend->packet);
		if (result == AVERROR(EAGAIN)) {
			backend->packet_pending = true;
			continue;
		}
		av_packet_unref(backend->packet);
		if (result < 0)
			return false;

		empty_reads = 0;
		end_of_stream_rewinds = 0;
	}
}

#endif
