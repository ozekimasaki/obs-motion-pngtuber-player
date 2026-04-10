#include "mpt-image-backend.h"

#include <new>

#if defined(_WIN32)

#include <wincodec.h>

struct MptImageBackend {
	IWICImagingFactory *wic_factory = nullptr;
};

namespace {

template<typename T>
void safe_release(T **ptr)
{
	if (ptr && *ptr) {
		(*ptr)->Release();
		*ptr = nullptr;
	}
}

} // namespace

bool mpt_image_backend_create(MptImageBackend **out_backend, std::string &error)
{
	if (out_backend)
		*out_backend = nullptr;
	if (!out_backend) {
		error = "invalid image backend output pointer";
		return false;
	}

	auto *backend = new (std::nothrow) MptImageBackend();
	if (!backend) {
		error = "out of memory while creating image backend";
		return false;
	}

	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&backend->wic_factory));
	if (FAILED(hr) || !backend->wic_factory) {
		delete backend;
		error = "failed to create WIC imaging factory";
		return false;
	}

	*out_backend = backend;
	return true;
}

void mpt_image_backend_destroy(MptImageBackend *backend)
{
	if (!backend)
		return;

	safe_release(&backend->wic_factory);
	delete backend;
}

ImageBGRA mpt_image_backend_load_png_bgra(MptImageBackend *backend, const std::filesystem::path &path, std::string &error)
{
	ImageBGRA out;

	if (!backend || !backend->wic_factory) {
		error = "image backend is not initialized";
		return out;
	}
	if (!std::filesystem::exists(path)) {
		error = std::string("missing mouth sprite: ") + path.u8string();
		return out;
	}

	IWICBitmapDecoder *decoder = nullptr;
	HRESULT hr =
		backend->wic_factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
	if (FAILED(hr) || !decoder) {
		error = "failed to open PNG sprite";
		return out;
	}

	IWICBitmapFrameDecode *frame = nullptr;
	hr = decoder->GetFrame(0, &frame);
	if (FAILED(hr) || !frame) {
		safe_release(&decoder);
		error = "failed to read PNG frame";
		return out;
	}

	IWICFormatConverter *converter = nullptr;
	hr = backend->wic_factory->CreateFormatConverter(&converter);
	if (FAILED(hr) || !converter) {
		safe_release(&frame);
		safe_release(&decoder);
		error = "failed to create PNG converter";
		return out;
	}

	hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
				   WICBitmapPaletteTypeCustom);
	if (FAILED(hr)) {
		safe_release(&converter);
		safe_release(&frame);
		safe_release(&decoder);
		error = "failed to convert PNG sprite to BGRA";
		return out;
	}

	UINT width = 0;
	UINT height = 0;
	converter->GetSize(&width, &height);
	out.width = width;
	out.height = height;
	out.pixels.resize(static_cast<size_t>(width) * height * 4U);
	hr = converter->CopyPixels(nullptr, width * 4U, static_cast<UINT>(out.pixels.size()), out.pixels.data());

	safe_release(&converter);
	safe_release(&frame);
	safe_release(&decoder);

	if (FAILED(hr)) {
		error = "failed to copy PNG pixels";
		return ImageBGRA();
	}

	return out;
}

#else

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

struct MptImageBackend {
	int unused = 0;
};

namespace {

static std::string ffmpeg_error_string(int error_code)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(error_code, buffer, sizeof(buffer));
	return std::string(buffer);
}

} // namespace

bool mpt_image_backend_create(MptImageBackend **out_backend, std::string &error)
{
	if (out_backend)
		*out_backend = nullptr;
	if (!out_backend) {
		error = "invalid image backend output pointer";
		return false;
	}

	auto *backend = new (std::nothrow) MptImageBackend();
	if (!backend) {
		error = "out of memory while creating image backend";
		return false;
	}

	*out_backend = backend;
	return true;
}

void mpt_image_backend_destroy(MptImageBackend *backend)
{
	delete backend;
}

ImageBGRA mpt_image_backend_load_png_bgra(MptImageBackend *backend, const std::filesystem::path &path, std::string &error)
{
	ImageBGRA out;
	if (!backend) {
		error = "image backend is not initialized";
		return out;
	}
	if (!std::filesystem::exists(path)) {
		error = std::string("missing mouth sprite: ") + path.u8string();
		return out;
	}

	AVFormatContext *format_context = nullptr;
	AVCodecContext *codec_context = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *packet = nullptr;
	SwsContext *sws_context = nullptr;

	int result = avformat_open_input(&format_context, path.u8string().c_str(), nullptr, nullptr);
	if (result < 0) {
		error = "failed to open PNG sprite: " + ffmpeg_error_string(result);
		goto cleanup;
	}

	result = avformat_find_stream_info(format_context, nullptr);
	if (result < 0) {
		error = "failed to inspect PNG sprite: " + ffmpeg_error_string(result);
		goto cleanup;
	}

	result = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (result < 0) {
		error = "failed to find a decodable PNG frame";
		goto cleanup;
	}

	{
		AVStream *stream = format_context->streams[result];
		const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
		if (!codec) {
			error = "failed to find a decoder for the PNG sprite";
			goto cleanup;
		}

		codec_context = avcodec_alloc_context3(codec);
		if (!codec_context) {
			error = "failed to allocate the PNG decoder";
			goto cleanup;
		}

		result = avcodec_parameters_to_context(codec_context, stream->codecpar);
		if (result < 0) {
			error = "failed to copy PNG codec parameters: " + ffmpeg_error_string(result);
			goto cleanup;
		}

		result = avcodec_open2(codec_context, codec, nullptr);
		if (result < 0) {
			error = "failed to open the PNG decoder: " + ffmpeg_error_string(result);
			goto cleanup;
		}
	}

	frame = av_frame_alloc();
	packet = av_packet_alloc();
	if (!frame || !packet) {
		error = "failed to allocate PNG decode buffers";
		goto cleanup;
	}

	while ((result = av_read_frame(format_context, packet)) >= 0) {
		const int stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (packet->stream_index != stream_index) {
			av_packet_unref(packet);
			continue;
		}

		result = avcodec_send_packet(codec_context, packet);
		av_packet_unref(packet);
		if (result < 0) {
			error = "failed to feed the PNG decoder: " + ffmpeg_error_string(result);
			goto cleanup;
		}

		result = avcodec_receive_frame(codec_context, frame);
		if (result == AVERROR(EAGAIN))
			continue;
		if (result < 0) {
			error = "failed to decode the PNG sprite: " + ffmpeg_error_string(result);
			goto cleanup;
		}

		sws_context = sws_getContext(codec_context->width, codec_context->height, codec_context->pix_fmt, codec_context->width,
					     codec_context->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!sws_context) {
			error = "failed to convert the PNG sprite to BGRA";
			goto cleanup;
		}

		out.width = (uint32_t)codec_context->width;
		out.height = (uint32_t)codec_context->height;
		out.pixels.assign((size_t)out.width * out.height * 4U, 0);
		uint8_t *dst_data[4] = {out.pixels.data(), nullptr, nullptr, nullptr};
		int dst_linesize[4] = {(int)out.stride(), 0, 0, 0};
		sws_scale(sws_context, frame->data, frame->linesize, 0, codec_context->height, dst_data, dst_linesize);
		error.clear();
		goto cleanup;
	}

	error = "failed to decode the PNG sprite";

cleanup:
	if (sws_context)
		sws_freeContext(sws_context);
	if (packet)
		av_packet_free(&packet);
	if (frame)
		av_frame_free(&frame);
	if (codec_context)
		avcodec_free_context(&codec_context);
	if (format_context)
		avformat_close_input(&format_context);
	if (!error.empty())
		return ImageBGRA();
	return out;
}

#endif
