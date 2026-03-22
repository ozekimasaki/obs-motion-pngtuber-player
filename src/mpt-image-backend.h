#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct ImageBGRA {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> pixels;

	bool empty() const
	{
		return pixels.empty() || width == 0 || height == 0;
	}

	size_t stride() const
	{
		return static_cast<size_t>(width) * 4U;
	}
};

struct MptImageBackend;

bool mpt_image_backend_create(MptImageBackend **out_backend, std::string &error);
void mpt_image_backend_destroy(MptImageBackend *backend);
ImageBGRA mpt_image_backend_load_png_bgra(MptImageBackend *backend, const std::filesystem::path &path, std::string &error);
