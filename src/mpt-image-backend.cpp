#include "mpt-image-backend.h"

#include <cstdio>
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

	std::string path_utf8 = path.u8string();
	FILE *file = fopen(path_utf8.c_str(), "rb");
	if (!file) {
		error = "failed to open PNG sprite";
		return out;
	}

	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!png) {
		fclose(file);
		error = "failed to create PNG decoder";
		return out;
	}

	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_read_struct(&png, nullptr, nullptr);
		fclose(file);
		error = "failed to create PNG info structure";
		return out;
	}

	if (setjmp(png_jmpbuf(png))) {
		png_destroy_read_struct(&png, &info, nullptr);
		fclose(file);
		error = "failed to decode PNG sprite";
		return ImageBGRA();
	}

	png_init_io(png, file);
	png_read_info(png, info);

	png_uint_32 width = 0;
	png_uint_32 height = 0;
	int bit_depth = 0;
	int color_type = 0;
	int interlace_type = 0;
	int compression_type = 0;
	int filter_method = 0;
	png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, &interlace_type, &compression_type, &filter_method);

	if (bit_depth == 16)
		png_set_strip_16(png);
	if (color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png);
	if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
		png_set_expand_gray_1_2_4_to_8(png);
	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);
	if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png);
	if ((color_type & PNG_COLOR_MASK_ALPHA) == 0)
		png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
	png_set_bgr(png);

	png_read_update_info(png, info);

	out.width = width;
	out.height = height;
	out.pixels.resize(static_cast<size_t>(width) * height * 4U);
	std::vector<png_bytep> rows(height);
	for (png_uint_32 y = 0; y < height; ++y)
		rows[y] = out.pixels.data() + static_cast<size_t>(y) * out.width * 4U;

	png_read_image(png, rows.data());
	png_read_end(png, info);
	png_destroy_read_struct(&png, &info, nullptr);
	fclose(file);
	return out;
#endif
}
