#include "mpt-video-backend.h"

#include <cstring>
#include <new>
#include <string>

#ifdef _WIN32
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <propidl.h>
#else
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

struct MptVideoBackend {
#ifdef _WIN32
	IMFSourceReader *reader = nullptr;
	bool mf_started = false;
#else
	AVFormatContext *format_context = nullptr;
	AVCodecContext *codec_context = nullptr;
	AVFrame *decoded_frame = nullptr;
	AVPacket *packet = nullptr;
	SwsContext *sws_context = nullptr;
	int video_stream_index = -1;
	AVRational stream_time_base {0, 1};
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

#else

static std::string ffmpeg_error_string(int error_code)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(error_code, buffer, sizeof(buffer));
	return buffer[0] ? std::string(buffer) : std::string("FFmpeg error");
}

static void close_video_state(MptVideoBackend *backend)
{
	if (!backend)
		return;

	if (backend->packet) {
		av_packet_free(&backend->packet);
		backend->packet = nullptr;
	}
	if (backend->decoded_frame) {
		av_frame_free(&backend->decoded_frame);
		backend->decoded_frame = nullptr;
	}
	if (backend->codec_context) {
		avcodec_free_context(&backend->codec_context);
		backend->codec_context = nullptr;
	}
	if (backend->format_context) {
		avformat_close_input(&backend->format_context);
		backend->format_context = nullptr;
	}
	if (backend->sws_context) {
		sws_freeContext(backend->sws_context);
		backend->sws_context = nullptr;
	}

	backend->video_stream_index = -1;
	backend->stream_time_base = AVRational{0, 1};
}

static bool ensure_output_image(ImageBGRA &image, int width, int height)
{
	if (width <= 0 || height <= 0)
		return false;

	uint32_t out_width = static_cast<uint32_t>(width);
	uint32_t out_height = static_cast<uint32_t>(height);
	size_t expected_size = static_cast<size_t>(out_width) * out_height * 4U;
	if (image.width != out_width || image.height != out_height || image.pixels.size() != expected_size) {
		image.width = out_width;
		image.height = out_height;
		image.pixels.assign(expected_size, 0);
	}
	return true;
}

static bool convert_frame_to_bgra(MptVideoBackend *backend, const AVFrame *frame, ImageBGRA &image)
{
	if (!backend || !frame)
		return false;
	if (!ensure_output_image(image, frame->width, frame->height))
		return false;

	backend->sws_context = sws_getCachedContext(backend->sws_context, frame->width, frame->height,
						    static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
						    AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!backend->sws_context)
		return false;

	uint8_t *dst_data[4] = {image.pixels.data(), nullptr, nullptr, nullptr};
	int dst_linesize[4] = {(int)image.stride(), 0, 0, 0};
	int scaled_rows = sws_scale(backend->sws_context, frame->data, frame->linesize, 0, frame->height, dst_data, dst_linesize);
	return scaled_rows == frame->height;
}

static bool seek_video_to_start(MptVideoBackend *backend)
{
	if (!backend || !backend->format_context || !backend->codec_context || backend->video_stream_index < 0)
		return false;

	int ret = av_seek_frame(backend->format_context, backend->video_stream_index, 0, AVSEEK_FLAG_BACKWARD);
	if (ret < 0)
		return false;

	avcodec_flush_buffers(backend->codec_context);
	return true;
}

static bool decode_ready_frame(MptVideoBackend *backend, ImageBGRA &image, uint64_t &timestamp_ns)
{
	for (;;) {
		int ret = avcodec_receive_frame(backend->codec_context, backend->decoded_frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return false;
		if (ret < 0)
			return false;

		if (!convert_frame_to_bgra(backend, backend->decoded_frame, image)) {
			av_frame_unref(backend->decoded_frame);
			return false;
		}

		int64_t pts = backend->decoded_frame->best_effort_timestamp;
		if (pts == AV_NOPTS_VALUE)
			pts = backend->decoded_frame->pts;
		if (pts == AV_NOPTS_VALUE || pts < 0) {
			timestamp_ns = 0;
		} else {
			timestamp_ns =
				static_cast<uint64_t>(av_rescale_q(pts, backend->stream_time_base, AVRational{1, 1000000000}));
		}

		av_frame_unref(backend->decoded_frame);
		return true;
	}
}

static bool decode_next_frame(MptVideoBackend *backend, ImageBGRA &image, uint64_t &timestamp_ns)
{
	if (!backend || !backend->format_context || !backend->codec_context || !backend->decoded_frame || !backend->packet)
		return false;

	for (;;) {
		int ret = av_read_frame(backend->format_context, backend->packet);
		if (ret == AVERROR_EOF) {
			ret = avcodec_send_packet(backend->codec_context, nullptr);
			if (ret >= 0 && decode_ready_frame(backend, image, timestamp_ns))
				return true;
			if (!seek_video_to_start(backend))
				return false;
			continue;
		}
		if (ret < 0)
			return false;

		if (backend->packet->stream_index != backend->video_stream_index) {
			av_packet_unref(backend->packet);
			continue;
		}

		ret = avcodec_send_packet(backend->codec_context, backend->packet);
		av_packet_unref(backend->packet);
		if (ret < 0)
			return false;
		if (decode_ready_frame(backend, image, timestamp_ns))
			return true;
	}
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

	auto *backend = new (std::nothrow) MptVideoBackend();
	if (!backend) {
		error = "out of memory while creating video backend";
		return false;
	}

#ifdef _WIN32
	HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
	if (FAILED(hr)) {
		delete backend;
		error = "MFStartup failed";
		return false;
	}

	backend->mf_started = true;
#endif

	*out_backend = backend;
	return true;
}

void mpt_video_backend_destroy(MptVideoBackend *backend)
{
	if (!backend)
		return;

#ifdef _WIN32
	safe_release(&backend->reader);
	if (backend->mf_started)
		MFShutdown();
#else
	close_video_state(backend);
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
	if (!backend) {
		error = "video backend is not initialized";
		return false;
	}
	if (loop_video_path.empty()) {
		error = "Loop video is required.";
		return false;
	}

	close_video_state(backend);

	int ret = avformat_open_input(&backend->format_context, loop_video_path.c_str(), nullptr, nullptr);
	if (ret < 0) {
		error = std::string("failed to open loop video: ") + ffmpeg_error_string(ret);
		close_video_state(backend);
		return false;
	}

	ret = avformat_find_stream_info(backend->format_context, nullptr);
	if (ret < 0) {
		error = std::string("failed to read video stream info: ") + ffmpeg_error_string(ret);
		close_video_state(backend);
		return false;
	}

	ret = av_find_best_stream(backend->format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (ret < 0) {
		error = std::string("failed to find a video stream: ") + ffmpeg_error_string(ret);
		close_video_state(backend);
		return false;
	}
	backend->video_stream_index = ret;

	AVStream *stream = backend->format_context->streams[backend->video_stream_index];
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec) {
		error = "failed to find a decoder for the loop video";
		close_video_state(backend);
		return false;
	}

	backend->codec_context = avcodec_alloc_context3(codec);
	if (!backend->codec_context) {
		error = "failed to allocate a video decoder";
		close_video_state(backend);
		return false;
	}

	ret = avcodec_parameters_to_context(backend->codec_context, stream->codecpar);
	if (ret < 0) {
		error = std::string("failed to copy video stream parameters: ") + ffmpeg_error_string(ret);
		close_video_state(backend);
		return false;
	}

	ret = avcodec_open2(backend->codec_context, codec, nullptr);
	if (ret < 0) {
		error = std::string("failed to open the video decoder: ") + ffmpeg_error_string(ret);
		close_video_state(backend);
		return false;
	}

	backend->decoded_frame = av_frame_alloc();
	backend->packet = av_packet_alloc();
	if (!backend->decoded_frame || !backend->packet) {
		error = "failed to allocate FFmpeg frame buffers";
		close_video_state(backend);
		return false;
	}

	backend->stream_time_base = stream->time_base;
	int initial_width = backend->codec_context->width > 0 ? backend->codec_context->width : stream->codecpar->width;
	int initial_height = backend->codec_context->height > 0 ? backend->codec_context->height : stream->codecpar->height;
	if (!ensure_output_image(*out_frame, initial_width, initial_height)) {
		error = "video dimensions are invalid";
		close_video_state(backend);
		return false;
	}

	return true;
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
	return decode_next_frame(backend, image, timestamp_ns);
#endif
}
