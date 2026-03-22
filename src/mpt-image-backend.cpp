#include "mpt-image-backend.h"

#include <new>

#ifdef _WIN32
#include <wincodec.h>
#else
#include <png.h>
#endif

struct MptImageBackend {
#ifdef _WIN32
	IWICImagingFactory *wic_factory = nullptr;
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

#endif

} // namespace

bool mpt_image_backend_create(MptImageBackend **out_backend, std::string &error)
{
	if (out_backend)
		*out_backend = nullptr;
	if (!out_backend) {
		error = "invalid image backend output pointer";
		return false;
	}

#ifdef _WIN32
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
#else
	auto *backend = new (std::nothrow) MptImageBackend();
	if (!backend) {
		error = "out of memory while creating image backend";
		return false;
	}

	*out_backend = backend;
	return true;
#endif
}

void mpt_image_backend_destroy(MptImageBackend *backend)
{
	if (!backend)
		return;

#ifdef _WIN32
	safe_release(&backend->wic_factory);
#endif

	delete backend;
}

ImageBGRA mpt_image_backend_load_png_bgra(MptImageBackend *backend, const std::filesystem::path &path, std::string &error)
{
	ImageBGRA out;

#ifdef _WIN32
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
#else
	if (!backend) {
		error = "image backend is not initialized";
		return out;
	}
	if (!std::filesystem::exists(path)) {
		error = std::string("missing mouth sprite: ") + path.u8string();
		return out;
	}

	png_image image {};
	image.version = PNG_IMAGE_VERSION;
	std::string path_utf8 = path.u8string();
	if (!png_image_begin_read_from_file(&image, path_utf8.c_str())) {
		error = image.message[0] ? image.message : "failed to open PNG sprite";
		png_image_free(&image);
		return out;
	}

	image.format = PNG_FORMAT_BGRA;
	out.width = image.width;
	out.height = image.height;
	out.pixels.resize(PNG_IMAGE_SIZE(image));
	if (!png_image_finish_read(&image, nullptr, out.pixels.data(), 0, nullptr)) {
		error = image.message[0] ? image.message : "failed to decode PNG sprite";
		png_image_free(&image);
		return ImageBGRA();
	}

	png_image_free(&image);
	return out;
#endif
}
