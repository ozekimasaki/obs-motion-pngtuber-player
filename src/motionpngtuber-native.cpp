#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "motionpngtuber-native.h"
#include "mpt-audio-backend.h"
#include "mpt-image-backend.h"
#include "mpt-text.h"
#include "mpt-video-backend.h"
#include "plugin-support.h"

#ifndef MPT_FALLBACK_OBS
#include <util/platform.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <objbase.h>

struct mpt_native_runtime {
	void *impl;
};

namespace {

struct PointF {
	float x = 0.0f;
	float y = 0.0f;
};

struct QuadFrame {
	std::array<PointF, 4> points {};
	std::array<double, 9> inv_homography {};
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	bool valid = false;
	bool warp_ready = false;
};

struct AudioAnalysisWindow {
	uint64_t start_timestamp_ns = 0;
	uint64_t end_timestamp_ns = 0;
	float rms = 0.0f;
	float zcr = 0.0f;
};

constexpr const char *kAutoObsAudioSourceSelection = "__auto__";
constexpr const char *kDirectInputAudioSourceSelection = "__direct__";

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

static std::filesystem::path utf8_to_path(const char *text)
{
	if (!text || !*text)
		return std::filesystem::path();
	return std::filesystem::path(utf8_to_wide(text));
}

static std::filesystem::path utf8_to_path(const std::string &text)
{
	return utf8_to_path(text.c_str());
}

static std::string to_lower_ascii(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return (char)tolower(ch); });
	return text;
}

static bool has_text(const char *value)
{
	return value && *value;
}

static bool is_json_path(const char *path)
{
	if (!has_text(path))
		return false;
	std::filesystem::path fs_path = utf8_to_path(path);
	return to_lower_ascii(fs_path.extension().u8string()) == ".json";
}

static bool is_npz_path(const char *path)
{
	if (!has_text(path))
		return false;
	std::filesystem::path fs_path = utf8_to_path(path);
	return to_lower_ascii(fs_path.extension().u8string()) == ".npz";
}

static bool file_exists_utf8(const std::string &path)
{
	if (path.empty())
		return false;
	return std::filesystem::is_regular_file(utf8_to_path(path));
}

static std::string resolve_track_json_fallback_candidate(const std::string &path)
{
	if (path.empty())
		return std::string();
	if (is_json_path(path.c_str()) && file_exists_utf8(path))
		return path;
	if (!is_npz_path(path.c_str()))
		return std::string();

	std::filesystem::path base = utf8_to_path(path);
	std::vector<std::filesystem::path> candidates;
	std::string stem = base.stem().u8string();
	candidates.push_back(base.parent_path() / (stem + ".json"));
	if (stem.find("_calibrated") != std::string::npos)
		candidates.push_back(base.parent_path() / "mouth_track.json");
	if (stem != "mouth_track")
		candidates.push_back(base.parent_path() / "mouth_track.json");

	for (const auto &candidate : candidates) {
		if (std::filesystem::is_regular_file(candidate))
			return candidate.u8string();
	}
	return std::string();
}

static std::string resolve_track_input_candidate(const std::string &path)
{
	if (path.empty())
		return std::string();
	if ((is_json_path(path.c_str()) || is_npz_path(path.c_str())) && file_exists_utf8(path))
		return path;
	return resolve_track_json_fallback_candidate(path);
}

static char *dup_error(const std::string &message)
{
	return bstrdup(message.empty() ? "native runtime error" : message.c_str());
}

static bool compute_homography(const std::array<PointF, 4> &src, const std::array<PointF, 4> &dst, double h[9]);
static bool invert_homography(const double in[9], double out[9]);
static bool prepare_quad_frame(QuadFrame &quad, uint32_t dst_width, uint32_t dst_height);

static void sample_bgra(const ImageBGRA &image, float src_x, float src_y, uint8_t &b, uint8_t &g, uint8_t &r, uint8_t &a)
{
	if (image.empty())
	{
		b = 0;
		g = 0;
		r = 0;
		a = 0;
		return;
	}

	src_x = std::clamp(src_x, 0.0f, (float)image.width - 1.0f);
	src_y = std::clamp(src_y, 0.0f, (float)image.height - 1.0f);

	int x0 = (int)floorf(src_x);
	int y0 = (int)floorf(src_y);
	int x1 = std::min(x0 + 1, (int)image.width - 1);
	int y1 = std::min(y0 + 1, (int)image.height - 1);

	float tx = src_x - (float)x0;
	float ty = src_y - (float)y0;
	float w00 = (1.0f - tx) * (1.0f - ty);
	float w10 = tx * (1.0f - ty);
	float w01 = (1.0f - tx) * ty;
	float w11 = tx * ty;

	const uint8_t *p00 = &image.pixels[(static_cast<size_t>(y0) * image.width + (size_t)x0) * 4U];
	const uint8_t *p10 = &image.pixels[(static_cast<size_t>(y0) * image.width + (size_t)x1) * 4U];
	const uint8_t *p01 = &image.pixels[(static_cast<size_t>(y1) * image.width + (size_t)x0) * 4U];
	const uint8_t *p11 = &image.pixels[(static_cast<size_t>(y1) * image.width + (size_t)x1) * 4U];

	auto blend = [&](size_t channel) -> uint8_t {
		float value = p00[channel] * w00 + p10[channel] * w10 + p01[channel] * w01 + p11[channel] * w11;
		return (uint8_t)std::clamp((int)lroundf(value), 0, 255);
	};

	b = blend(0);
	g = blend(1);
	r = blend(2);
	a = blend(3);
}

static ImageBGRA resize_image(const ImageBGRA &src, uint32_t dst_width, uint32_t dst_height)
{
	ImageBGRA out;
	out.width = dst_width;
	out.height = dst_height;
	out.pixels.resize((size_t)dst_width * dst_height * 4U);
	if (src.empty() || dst_width == 0 || dst_height == 0)
		return out;

	float x_scale = dst_width > 1 ? (float)(src.width - 1) / (float)(dst_width - 1) : 0.0f;
	float y_scale = dst_height > 1 ? (float)(src.height - 1) / (float)(dst_height - 1) : 0.0f;

	for (uint32_t y = 0; y < dst_height; ++y) {
		for (uint32_t x = 0; x < dst_width; ++x) {
			float sx = x_scale * (float)x;
			float sy = y_scale * (float)y;
			size_t offset = ((size_t)y * dst_width + x) * 4U;
			uint8_t b = 0;
			uint8_t g = 0;
			uint8_t r = 0;
			uint8_t a = 0;
			sample_bgra(src, sx, sy, b, g, r, a);
			out.pixels[offset + 0] = b;
			out.pixels[offset + 1] = g;
			out.pixels[offset + 2] = r;
			out.pixels[offset + 3] = a;
		}
	}

	return out;
}

static void alpha_blit(ImageBGRA &dst, const ImageBGRA &src, int x, int y)
{
	if (dst.empty() || src.empty())
		return;

	int x0 = std::max(x, 0);
	int y0 = std::max(y, 0);
	int x1 = std::min(x + (int)src.width, (int)dst.width);
	int y1 = std::min(y + (int)src.height, (int)dst.height);
	if (x0 >= x1 || y0 >= y1)
		return;

	for (int py = y0; py < y1; ++py) {
		for (int px = x0; px < x1; ++px) {
			size_t dst_offset = ((size_t)py * dst.width + (size_t)px) * 4U;
			size_t src_offset =
				((size_t)(py - y) * src.width + (size_t)(px - x)) * 4U;
			uint16_t alpha = src.pixels[src_offset + 3];
			if (alpha == 0)
				continue;
			uint16_t inv = (uint16_t)(255 - alpha);
			dst.pixels[dst_offset + 0] =
				(uint8_t)((src.pixels[src_offset + 0] * alpha + dst.pixels[dst_offset + 0] * inv) / 255);
			dst.pixels[dst_offset + 1] =
				(uint8_t)((src.pixels[src_offset + 1] * alpha + dst.pixels[dst_offset + 1] * inv) / 255);
			dst.pixels[dst_offset + 2] =
				(uint8_t)((src.pixels[src_offset + 2] * alpha + dst.pixels[dst_offset + 2] * inv) / 255);
			dst.pixels[dst_offset + 3] = 255;
		}
	}
}

static ImageBGRA crop_to_alpha(const ImageBGRA &src, uint32_t full_width, uint32_t full_height)
{
	if (src.empty())
		return src;
	if (src.width != full_width || src.height != full_height)
		return src;

	uint32_t min_x = src.width;
	uint32_t min_y = src.height;
	uint32_t max_x = 0;
	uint32_t max_y = 0;
	bool found = false;

	for (uint32_t y = 0; y < src.height; ++y) {
		for (uint32_t x = 0; x < src.width; ++x) {
			size_t offset = ((size_t)y * src.width + x) * 4U + 3U;
			if (src.pixels[offset] <= 8)
				continue;
			min_x = std::min(min_x, x);
			min_y = std::min(min_y, y);
			max_x = std::max(max_x, x);
			max_y = std::max(max_y, y);
			found = true;
		}
	}

	if (!found)
		return src;

	ImageBGRA out;
	out.width = max_x - min_x + 1U;
	out.height = max_y - min_y + 1U;
	out.pixels.resize((size_t)out.width * out.height * 4U);
	for (uint32_t y = 0; y < out.height; ++y) {
		const uint8_t *src_row = &src.pixels[((size_t)(min_y + y) * src.width + min_x) * 4U];
		uint8_t *dst_row = &out.pixels[(size_t)y * out.width * 4U];
		memcpy(dst_row, src_row, (size_t)out.width * 4U);
	}
	return out;
}

static ImageBGRA make_variant_from_open(const ImageBGRA &open_image, const char *key)
{
	if (open_image.empty())
		return open_image;

	float sx = 1.0f;
	float sy = 1.0f;
	if (strcmp(key, "half") == 0) {
		sy = 0.65f;
	} else if (strcmp(key, "closed") == 0) {
		sy = 0.25f;
	} else if (strcmp(key, "u") == 0) {
		sx = 0.88f;
		sy = 0.55f;
	} else if (strcmp(key, "e") == 0) {
		sx = 1.08f;
		sy = 0.55f;
	}

	uint32_t resized_width = std::max(2U, (uint32_t)lroundf((float)open_image.width * sx));
	uint32_t resized_height = std::max(2U, (uint32_t)lroundf((float)open_image.height * sy));
	ImageBGRA scaled_image = resize_image(open_image, resized_width, resized_height);

	ImageBGRA canvas;
	canvas.width = open_image.width;
	canvas.height = open_image.height;
	canvas.pixels.assign((size_t)canvas.width * canvas.height * 4U, 0);

	int x = ((int)canvas.width - (int)scaled_image.width) / 2;
	int y = (int)canvas.height - (int)scaled_image.height;
	alpha_blit(canvas, scaled_image, x, y);

	if (strcmp(key, "closed") == 0) {
		for (size_t idx = 3; idx < canvas.pixels.size(); idx += 4)
			canvas.pixels[idx] = (uint8_t)std::clamp((int)lroundf((float)canvas.pixels[idx] * 0.85f), 0, 255);
	}

	return canvas;
}

static bool solve_linear_system_8x8(double matrix[8][9])
{
	for (int col = 0; col < 8; ++col) {
		int pivot = col;
		for (int row = col + 1; row < 8; ++row) {
			if (fabs(matrix[row][col]) > fabs(matrix[pivot][col]))
				pivot = row;
		}
		if (fabs(matrix[pivot][col]) < 1e-9)
			return false;
		if (pivot != col) {
			for (int k = col; k < 9; ++k)
				std::swap(matrix[pivot][k], matrix[col][k]);
		}
		double div = matrix[col][col];
		for (int k = col; k < 9; ++k)
			matrix[col][k] /= div;
		for (int row = 0; row < 8; ++row) {
			if (row == col)
				continue;
			double factor = matrix[row][col];
			for (int k = col; k < 9; ++k)
				matrix[row][k] -= factor * matrix[col][k];
		}
	}
	return true;
}

static bool compute_homography(const std::array<PointF, 4> &src, const std::array<PointF, 4> &dst, double h[9])
{
	double matrix[8][9] = {};
	for (int i = 0; i < 4; ++i) {
		double x = src[(size_t)i].x;
		double y = src[(size_t)i].y;
		double u = dst[(size_t)i].x;
		double v = dst[(size_t)i].y;

		matrix[i * 2 + 0][0] = x;
		matrix[i * 2 + 0][1] = y;
		matrix[i * 2 + 0][2] = 1.0;
		matrix[i * 2 + 0][6] = -u * x;
		matrix[i * 2 + 0][7] = -u * y;
		matrix[i * 2 + 0][8] = u;

		matrix[i * 2 + 1][3] = x;
		matrix[i * 2 + 1][4] = y;
		matrix[i * 2 + 1][5] = 1.0;
		matrix[i * 2 + 1][6] = -v * x;
		matrix[i * 2 + 1][7] = -v * y;
		matrix[i * 2 + 1][8] = v;
	}

	if (!solve_linear_system_8x8(matrix))
		return false;

	h[0] = matrix[0][8];
	h[1] = matrix[1][8];
	h[2] = matrix[2][8];
	h[3] = matrix[3][8];
	h[4] = matrix[4][8];
	h[5] = matrix[5][8];
	h[6] = matrix[6][8];
	h[7] = matrix[7][8];
	h[8] = 1.0;
	return true;
}

static bool invert_homography(const double in[9], double out[9])
{
	double det = in[0] * (in[4] * in[8] - in[5] * in[7]) - in[1] * (in[3] * in[8] - in[5] * in[6]) +
		     in[2] * (in[3] * in[7] - in[4] * in[6]);
	if (fabs(det) < 1e-12)
		return false;

	double inv_det = 1.0 / det;
	out[0] = (in[4] * in[8] - in[5] * in[7]) * inv_det;
	out[1] = (in[2] * in[7] - in[1] * in[8]) * inv_det;
	out[2] = (in[1] * in[5] - in[2] * in[4]) * inv_det;
	out[3] = (in[5] * in[6] - in[3] * in[8]) * inv_det;
	out[4] = (in[0] * in[8] - in[2] * in[6]) * inv_det;
	out[5] = (in[2] * in[3] - in[0] * in[5]) * inv_det;
	out[6] = (in[3] * in[7] - in[4] * in[6]) * inv_det;
	out[7] = (in[1] * in[6] - in[0] * in[7]) * inv_det;
	out[8] = (in[0] * in[4] - in[1] * in[3]) * inv_det;
	return true;
}

static bool prepare_quad_frame(QuadFrame &quad, uint32_t dst_width, uint32_t dst_height)
{
	quad.warp_ready = false;
	if (!quad.valid)
		return false;

	float min_x = quad.points[0].x;
	float min_y = quad.points[0].y;
	float max_x = quad.points[0].x;
	float max_y = quad.points[0].y;
	for (size_t i = 1; i < 4; ++i) {
		min_x = std::min(min_x, quad.points[i].x);
		min_y = std::min(min_y, quad.points[i].y);
		max_x = std::max(max_x, quad.points[i].x);
		max_y = std::max(max_y, quad.points[i].y);
	}

	quad.x0 = std::max(0, (int)floorf(min_x));
	quad.y0 = std::max(0, (int)floorf(min_y));
	quad.x1 = std::min((int)dst_width, (int)ceilf(max_x));
	quad.y1 = std::min((int)dst_height, (int)ceilf(max_y));
	if (quad.x0 >= quad.x1 || quad.y0 >= quad.y1)
		return false;

	static const std::array<PointF, 4> src_points = {
		PointF{0.0f, 0.0f},
		PointF{1.0f, 0.0f},
		PointF{1.0f, 1.0f},
		PointF{0.0f, 1.0f},
	};
	double homography[9] = {};
	if (!compute_homography(src_points, quad.points, homography) ||
	    !invert_homography(homography, quad.inv_homography.data()))
		return false;

	quad.warp_ready = true;
	return true;
}

static void warp_and_blend(ImageBGRA &dst, const ImageBGRA &src, const QuadFrame &quad)
{
	if (dst.empty() || src.empty() || !quad.warp_ready)
		return;

	float src_max_x = (float)src.width - 1.0f;
	float src_max_y = (float)src.height - 1.0f;
	for (int y = quad.y0; y < quad.y1; ++y) {
		for (int x = quad.x0; x < quad.x1; ++x) {
			double dx = (double)x + 0.5;
			double dy = (double)y + 0.5;
			double denom = quad.inv_homography[6] * dx + quad.inv_homography[7] * dy + quad.inv_homography[8];
			if (fabs(denom) < 1e-12)
				continue;
			float u = (float)((quad.inv_homography[0] * dx + quad.inv_homography[1] * dy + quad.inv_homography[2]) / denom);
			float v = (float)((quad.inv_homography[3] * dx + quad.inv_homography[4] * dy + quad.inv_homography[5]) / denom);
			if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f)
				continue;

			uint8_t b = 0;
			uint8_t g = 0;
			uint8_t r = 0;
			uint8_t a = 0;
			sample_bgra(src, u * src_max_x, v * src_max_y, b, g, r, a);
			if (a == 0)
				continue;

			size_t dst_offset = ((size_t)y * dst.width + (size_t)x) * 4U;
			uint16_t alpha = a;
			uint16_t inv = (uint16_t)(255 - alpha);
			dst.pixels[dst_offset + 0] = (uint8_t)((b * alpha + dst.pixels[dst_offset + 0] * inv) / 255);
			dst.pixels[dst_offset + 1] = (uint8_t)((g * alpha + dst.pixels[dst_offset + 1] * inv) / 255);
			dst.pixels[dst_offset + 2] = (uint8_t)((r * alpha + dst.pixels[dst_offset + 2] * inv) / 255);
			dst.pixels[dst_offset + 3] = 255;
		}
	}
}

static void add_disabled_list_item(obs_property_t *list, const char *label)
{
	size_t idx = obs_property_list_add_string(list, label, "");
	obs_property_list_item_disable(list, idx, true);
}

static bool read_text_file_utf8(const std::string &path, std::string &text)
{
	std::ifstream input(utf8_to_path(path), std::ios::binary);
	if (!input)
		return false;
	std::ostringstream buffer;
	buffer << input.rdbuf();
	text = buffer.str();
	return true;
}

static bool read_binary_file_utf8(const std::string &path, std::vector<uint8_t> &bytes)
{
	std::ifstream input(utf8_to_path(path), std::ios::binary);
	if (!input)
		return false;

	input.seekg(0, std::ios::end);
	std::streamoff size = input.tellg();
	if (size < 0)
		return false;
	input.seekg(0, std::ios::beg);

	bytes.resize((size_t)size);
	if (size == 0)
		return true;

	input.read(reinterpret_cast<char *>(bytes.data()), size);
	return input.good() || input.eof();
}

struct NpzEntryView {
	std::string name;
	uint16_t compression_method = 0;
	const uint8_t *data = nullptr;
	size_t compressed_size = 0;
	size_t uncompressed_size = 0;
};

struct NpyArrayView {
	std::string descr;
	char byte_order = '|';
	char kind = '\0';
	size_t item_size = 0;
	bool fortran_order = false;
	std::vector<size_t> shape;
	const uint8_t *data = nullptr;
	size_t data_size = 0;
};

static uint16_t read_le16(const uint8_t *data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool npy_find_value_start(const std::string &header, const char *name, size_t &value_pos)
{
	if (!name || !*name)
		return false;

	std::string single_quote_key = std::string("'") + name + "'";
	size_t key_pos = header.find(single_quote_key);
	if (key_pos == std::string::npos) {
		std::string double_quote_key = std::string("\"") + name + "\"";
		key_pos = header.find(double_quote_key);
	}
	if (key_pos == std::string::npos)
		return false;

	size_t colon_pos = header.find(':', key_pos);
	if (colon_pos == std::string::npos)
		return false;

	value_pos = colon_pos + 1;
	while (value_pos < header.size() && std::isspace((unsigned char)header[value_pos]))
		++value_pos;
	return value_pos < header.size();
}

static bool npy_try_get_quoted_value(const std::string &header, const char *name, std::string &out)
{
	size_t value_pos = 0;
	if (!npy_find_value_start(header, name, value_pos))
		return false;
	if (value_pos >= header.size())
		return false;

	char quote = header[value_pos];
	if (quote != '\'' && quote != '"')
		return false;

	size_t end_pos = header.find(quote, value_pos + 1);
	if (end_pos == std::string::npos)
		return false;

	out = header.substr(value_pos + 1, end_pos - value_pos - 1);
	return true;
}

static bool npy_try_get_bool_value(const std::string &header, const char *name, bool &out)
{
	size_t value_pos = 0;
	if (!npy_find_value_start(header, name, value_pos))
		return false;
	if (header.compare(value_pos, 4, "True") == 0) {
		out = true;
		return true;
	}
	if (header.compare(value_pos, 5, "False") == 0) {
		out = false;
		return true;
	}
	return false;
}

static bool npy_try_get_shape_value(const std::string &header, std::vector<size_t> &shape)
{
	size_t value_pos = 0;
	if (!npy_find_value_start(header, "shape", value_pos))
		return false;
	if (value_pos >= header.size() || header[value_pos] != '(')
		return false;

	size_t end_pos = header.find(')', value_pos);
	if (end_pos == std::string::npos)
		return false;

	shape.clear();
	size_t cursor = value_pos + 1;
	while (cursor < end_pos) {
		while (cursor < end_pos && (std::isspace((unsigned char)header[cursor]) || header[cursor] == ','))
			++cursor;
		if (cursor >= end_pos)
			break;

		const char *start = header.c_str() + cursor;
		char *end = nullptr;
		unsigned long long dim = std::strtoull(start, &end, 10);
		if (start == end)
			return false;
		if (dim > std::numeric_limits<size_t>::max())
			return false;
		shape.push_back((size_t)dim);
		cursor = (size_t)(end - header.c_str());
	}

	return true;
}

static bool safe_multiply_size(size_t lhs, size_t rhs, size_t &out)
{
	if (lhs == 0 || rhs == 0) {
		out = 0;
		return true;
	}
	if (lhs > std::numeric_limits<size_t>::max() / rhs)
		return false;
	out = lhs * rhs;
	return true;
}

static bool npy_try_total_elements(const NpyArrayView &array, size_t &total)
{
	total = 1;
	for (size_t dim : array.shape) {
		if (!safe_multiply_size(total, dim, total))
			return false;
	}
	return true;
}

static const NpzEntryView *find_npz_entry(const std::vector<NpzEntryView> &entries, const char *name)
{
	for (const auto &entry : entries) {
		if (entry.name == name)
			return &entry;
	}
	return nullptr;
}

static bool parse_npz_entries(const std::vector<uint8_t> &archive, std::vector<NpzEntryView> &entries, std::string &error)
{
	entries.clear();
	size_t offset = 0;
	while (offset + 4U <= archive.size()) {
		uint32_t signature = read_le32(&archive[offset]);
		if (signature == 0x02014b50U || signature == 0x06054b50U)
			break;
		if (signature != 0x04034b50U) {
			error = "NPZ does not contain a valid ZIP local header";
			return false;
		}
		if (offset + 30U > archive.size()) {
			error = "NPZ local header is truncated";
			return false;
		}

		uint16_t flags = read_le16(&archive[offset + 6U]);
		uint16_t compression_method = read_le16(&archive[offset + 8U]);
		uint32_t compressed_size = read_le32(&archive[offset + 18U]);
		uint32_t uncompressed_size = read_le32(&archive[offset + 22U]);
		uint16_t name_length = read_le16(&archive[offset + 26U]);
		uint16_t extra_length = read_le16(&archive[offset + 28U]);

		if ((flags & 0x0008U) != 0) {
			error = "NPZ uses ZIP data descriptors, which are not supported";
			return false;
		}

		size_t name_offset = offset + 30U;
		size_t data_offset = name_offset + (size_t)name_length + (size_t)extra_length;
		if (name_offset + (size_t)name_length > archive.size() || data_offset > archive.size()) {
			error = "NPZ local header is truncated";
			return false;
		}
		if (data_offset + (size_t)compressed_size > archive.size()) {
			error = "NPZ member data is truncated";
			return false;
		}

		NpzEntryView entry;
		entry.name.assign(reinterpret_cast<const char *>(&archive[name_offset]), name_length);
		entry.compression_method = compression_method;
		entry.data = archive.data() + data_offset;
		entry.compressed_size = compressed_size;
		entry.uncompressed_size = uncompressed_size;
		entries.push_back(entry);

		offset = data_offset + (size_t)compressed_size;
	}

	if (entries.empty()) {
		error = "NPZ archive does not contain any readable members";
		return false;
	}
	return true;
}

static bool parse_npy_array(const NpzEntryView &entry, NpyArrayView &array, std::string &error)
{
	if (entry.compression_method != 0) {
		error = "compressed NPZ members are not supported yet";
		return false;
	}
	if (!entry.data || entry.compressed_size < 10U) {
		error = "NPY member is truncated";
		return false;
	}
	if (!(entry.data[0] == 0x93 && entry.data[1] == 'N' && entry.data[2] == 'U' && entry.data[3] == 'M' &&
	      entry.data[4] == 'P' && entry.data[5] == 'Y')) {
		error = "NPZ member is not an NPY array";
		return false;
	}

	uint8_t major = entry.data[6];
	size_t header_size_field = 0;
	size_t header_length = 0;
	if (major == 1) {
		header_size_field = 2;
		header_length = read_le16(entry.data + 8);
	} else if (major == 2 || major == 3) {
		if (entry.compressed_size < 12U) {
			error = "NPY member is truncated";
			return false;
		}
		header_size_field = 4;
		header_length = read_le32(entry.data + 8);
	} else {
		error = "unsupported NPY format version";
		return false;
	}

	size_t header_offset = 8U + header_size_field;
	if (header_length > entry.compressed_size - header_offset) {
		error = "NPY header is truncated";
		return false;
	}
	size_t data_offset = header_offset + header_length;

	std::string header(reinterpret_cast<const char *>(entry.data + header_offset), header_length);
	std::string descr;
	if (!npy_try_get_quoted_value(header, "descr", descr)) {
		error = "NPY header does not contain descr";
		return false;
	}

	bool fortran_order = false;
	if (!npy_try_get_bool_value(header, "fortran_order", fortran_order)) {
		error = "NPY header does not contain fortran_order";
		return false;
	}
	if (fortran_order) {
		error = "Fortran-ordered NPY arrays are not supported";
		return false;
	}

	std::vector<size_t> shape;
	if (!npy_try_get_shape_value(header, shape)) {
		error = "NPY header does not contain shape";
		return false;
	}
	if (descr.size() < 3U) {
		error = "NPY dtype descriptor is invalid";
		return false;
	}

	char byte_order = descr[0];
	char kind = descr[1];
	if (byte_order != '<' && byte_order != '|' && byte_order != '=') {
		error = "unsupported NPY byte order";
		return false;
	}

	unsigned long item_size = std::strtoul(descr.c_str() + 2, nullptr, 10);
	if (item_size == 0) {
		error = "NPY dtype item size is invalid";
		return false;
	}

	array.descr = descr;
	array.byte_order = byte_order;
	array.kind = kind;
	array.item_size = (size_t)item_size;
	array.fortran_order = false;
	array.shape = std::move(shape);
	array.data = entry.data + data_offset;

	size_t total_elements = 0;
	if (!npy_try_total_elements(array, total_elements)) {
		error = "NPY array is too large";
		return false;
	}

	size_t expected_bytes = 1;
	if (!safe_multiply_size(total_elements, array.item_size, expected_bytes)) {
		error = "NPY array is too large";
		return false;
	}
	if (expected_bytes > entry.compressed_size - data_offset) {
		error = "NPY payload is truncated";
		return false;
	}
	array.data_size = expected_bytes;
	return true;
}

template<typename T>
static T read_unaligned_value(const uint8_t *data)
{
	T value {};
	memcpy(&value, data, sizeof(T));
	return value;
}

static bool npy_read_number_at(const NpyArrayView &array, size_t index, double &out, std::string &error)
{
	size_t offset = 0;
	if (!safe_multiply_size(index, array.item_size, offset) || offset + array.item_size > array.data_size) {
		error = "NPY index is out of range";
		return false;
	}

	const uint8_t *ptr = array.data + offset;
	switch (array.kind) {
	case 'b':
		if (array.item_size == 1U) {
			out = ptr[0] ? 1.0 : 0.0;
			return true;
		}
		break;

	case 'u':
		switch (array.item_size) {
		case 1U:
			out = (double)ptr[0];
			return true;
		case 2U:
			out = (double)read_unaligned_value<uint16_t>(ptr);
			return true;
		case 4U:
			out = (double)read_unaligned_value<uint32_t>(ptr);
			return true;
		case 8U:
			out = (double)read_unaligned_value<uint64_t>(ptr);
			return true;
		}
		break;

	case 'i':
		switch (array.item_size) {
		case 1U:
			out = (double)read_unaligned_value<int8_t>(ptr);
			return true;
		case 2U:
			out = (double)read_unaligned_value<int16_t>(ptr);
			return true;
		case 4U:
			out = (double)read_unaligned_value<int32_t>(ptr);
			return true;
		case 8U:
			out = (double)read_unaligned_value<int64_t>(ptr);
			return true;
		}
		break;

	case 'f':
		switch (array.item_size) {
		case 4U:
			out = (double)read_unaligned_value<float>(ptr);
			return true;
		case 8U:
			out = read_unaligned_value<double>(ptr);
			return true;
		}
		break;
	}

	error = std::string("unsupported NPY dtype: ") + array.descr;
	return false;
}

static bool npy_read_numeric_values(const NpyArrayView &array, std::vector<double> &values, std::string &error)
{
	size_t total = 0;
	if (!npy_try_total_elements(array, total)) {
		error = "NPY array is too large";
		return false;
	}
	values.resize(total);
	for (size_t idx = 0; idx < total; ++idx) {
		if (!npy_read_number_at(array, idx, values[idx], error))
			return false;
	}
	return true;
}

static bool npy_read_scalar_number(const NpyArrayView &array, double &out, std::string &error)
{
	size_t total = 0;
	if (!npy_try_total_elements(array, total)) {
		error = "NPY array is too large";
		return false;
	}
	if (total == 0 || total > 1) {
		error = "NPY scalar array is not a scalar";
		return false;
	}
	return npy_read_number_at(array, 0, out, error);
}

static void populate_quad_from_bbox_values(const std::vector<double> &bbox_values, size_t frame_index, float sx, float sy,
					   QuadFrame &quad)
{
	size_t base = frame_index * 4U;
	double x = bbox_values[base + 0];
	double y = bbox_values[base + 1];
	double w = bbox_values[base + 2];
	double h = bbox_values[base + 3];
	quad.points[0].x = (float)x * sx;
	quad.points[0].y = (float)y * sy;
	quad.points[1].x = (float)(x + w) * sx;
	quad.points[1].y = (float)y * sy;
	quad.points[2].x = (float)(x + w) * sx;
	quad.points[2].y = (float)(y + h) * sy;
	quad.points[3].x = (float)x * sx;
	quad.points[3].y = (float)(y + h) * sy;
}

static bool load_track_frames_from_npz(const std::string &path, uint32_t output_width, uint32_t output_height,
				       std::vector<QuadFrame> &frames_out, bool &any_valid, std::string &error)
{
	std::vector<uint8_t> archive;
	if (!read_binary_file_utf8(path, archive)) {
		error = "failed to read track NPZ";
		return false;
	}

	std::vector<NpzEntryView> entries;
	if (!parse_npz_entries(archive, entries, error))
		return false;

	const NpzEntryView *quad_entry = find_npz_entry(entries, "quad.npy");
	const NpzEntryView *bbox_entry = find_npz_entry(entries, "bbox.npy");
	if (!quad_entry && !bbox_entry) {
		error = "track NPZ must contain quad.npy or bbox.npy";
		return false;
	}

	size_t frame_count = 0;
	std::vector<double> primary_values;
	if (quad_entry) {
		NpyArrayView quad_array;
		if (!parse_npy_array(*quad_entry, quad_array, error))
			return false;
		if (quad_array.shape.size() != 3U || quad_array.shape[1] != 4U || quad_array.shape[2] != 2U) {
			error = "track NPZ quad array must be shaped (N,4,2)";
			return false;
		}
		frame_count = quad_array.shape[0];
		if (!npy_read_numeric_values(quad_array, primary_values, error))
			return false;
	} else {
		NpyArrayView bbox_array;
		if (!parse_npy_array(*bbox_entry, bbox_array, error))
			return false;
		if (bbox_array.shape.size() != 2U || bbox_array.shape[1] != 4U) {
			error = "track NPZ bbox array must be shaped (N,4)";
			return false;
		}
		frame_count = bbox_array.shape[0];
		if (!npy_read_numeric_values(bbox_array, primary_values, error))
			return false;
	}

	std::vector<bool> valid(frame_count, true);
	if (const NpzEntryView *valid_entry = find_npz_entry(entries, "valid.npy")) {
		NpyArrayView valid_array;
		if (!parse_npy_array(*valid_entry, valid_array, error))
			return false;
		size_t valid_count = 0;
		if (!npy_try_total_elements(valid_array, valid_count)) {
			error = "NPY array is too large";
			return false;
		}
		if (valid_count == frame_count) {
			for (size_t idx = 0; idx < frame_count; ++idx) {
				double value = 0.0;
				if (!npy_read_number_at(valid_array, idx, value, error))
					return false;
				valid[idx] = value != 0.0;
			}
		}
	}

	double src_w = (double)output_width;
	if (const NpzEntryView *width_entry = find_npz_entry(entries, "w.npy")) {
		NpyArrayView width_array;
		if (!parse_npy_array(*width_entry, width_array, error))
			return false;
		if (!npy_read_scalar_number(width_array, src_w, error))
			return false;
	}

	double src_h = (double)output_height;
	if (const NpzEntryView *height_entry = find_npz_entry(entries, "h.npy")) {
		NpyArrayView height_array;
		if (!parse_npy_array(*height_entry, height_array, error))
			return false;
		if (!npy_read_scalar_number(height_array, src_h, error))
			return false;
	}

	float sx = (float)output_width / (float)std::max(1.0, src_w);
	float sy = (float)output_height / (float)std::max(1.0, src_h);
	frames_out.clear();
	frames_out.reserve(frame_count);
	any_valid = false;

	for (size_t idx = 0; idx < frame_count; ++idx) {
		QuadFrame quad {};
		quad.valid = valid[idx];
		if (quad_entry) {
			size_t base = idx * 8U;
			for (size_t point = 0; point < 4U; ++point) {
				quad.points[point].x = (float)primary_values[base + point * 2U + 0U] * sx;
				quad.points[point].y = (float)primary_values[base + point * 2U + 1U] * sy;
			}
		} else {
			populate_quad_from_bbox_values(primary_values, idx, sx, sy, quad);
		}
		prepare_quad_frame(quad, output_width, output_height);
		frames_out.push_back(quad);
		any_valid = any_valid || quad.valid;
	}

	if (frames_out.empty()) {
		error = "track NPZ did not produce any usable frames";
		return false;
	}
	return true;
}

static bool json_find_value_start(const std::string &json, const char *name, size_t &value_pos)
{
	if (!name || !*name)
		return false;

	bool in_string = false;
	bool escaped = false;
	size_t string_start = std::string::npos;
	size_t name_len = std::strlen(name);
	for (size_t idx = 0; idx < json.size(); ++idx) {
		char ch = json[idx];
		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				size_t content_start = string_start + 1;
				size_t content_len = idx - content_start;
				if (content_len == name_len && json.compare(content_start, content_len, name) == 0) {
					size_t pos = idx + 1;
					while (pos < json.size() && std::isspace((unsigned char)json[pos]))
						++pos;
					if (pos < json.size() && json[pos] == ':') {
						++pos;
						while (pos < json.size() && std::isspace((unsigned char)json[pos]))
							++pos;
						value_pos = pos;
						return true;
					}
				}
				in_string = false;
				string_start = std::string::npos;
			}
			continue;
		}

		if (ch == '"') {
			in_string = true;
			escaped = false;
			string_start = idx;
		}
	}
	return false;
}

static bool json_try_get_number(const std::string &json, const char *name, double &out)
{
	size_t value_pos = 0;
	if (!json_find_value_start(json, name, value_pos))
		return false;

	const char *start = json.c_str() + value_pos;
	char *end = nullptr;
	double value = std::strtod(start, &end);
	if (start == end)
		return false;
	out = value;
	return true;
}

static bool json_try_get_bool(const std::string &json, const char *name, bool &out)
{
	size_t value_pos = 0;
	if (!json_find_value_start(json, name, value_pos))
		return false;
	if (json.compare(value_pos, 4, "true") == 0) {
		out = true;
		return true;
	}
	if (json.compare(value_pos, 5, "false") == 0) {
		out = false;
		return true;
	}
	return false;
}

static bool json_extract_bracketed_text(const std::string &json, size_t start_pos, char open_ch, char close_ch, std::string &out)
{
	if (start_pos >= json.size() || json[start_pos] != open_ch)
		return false;

	bool in_string = false;
	bool escaped = false;
	int depth = 0;
	for (size_t idx = start_pos; idx < json.size(); ++idx) {
		char ch = json[idx];
		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				in_string = false;
			}
			continue;
		}

		if (ch == '"') {
			in_string = true;
			continue;
		}
		if (ch == open_ch) {
			++depth;
		} else if (ch == close_ch) {
			--depth;
			if (depth == 0) {
				out = json.substr(start_pos, idx - start_pos + 1);
				return true;
			}
		}
	}
	return false;
}

static bool json_try_get_array_text(const std::string &json, const char *name, std::string &array_text)
{
	size_t value_pos = 0;
	if (!json_find_value_start(json, name, value_pos))
		return false;
	return json_extract_bracketed_text(json, value_pos, '[', ']', array_text);
}

static void json_collect_numbers(const std::string &json, std::vector<double> &numbers)
{
	numbers.clear();
	size_t idx = 0;
	while (idx < json.size()) {
		unsigned char ch = (unsigned char)json[idx];
		if (std::isdigit(ch) || ch == '-' || ch == '+' || ch == '.') {
			char *end = nullptr;
			double value = std::strtod(json.c_str() + idx, &end);
			if (end && end != json.c_str() + idx) {
				numbers.push_back(value);
				idx = (size_t)(end - json.c_str());
				continue;
			}
		}
		++idx;
	}
}

static bool json_split_top_level_objects(const std::string &array_text, std::vector<std::string> &objects)
{
	objects.clear();
	if (array_text.empty() || array_text.front() != '[')
		return false;

	bool in_string = false;
	bool escaped = false;
	int object_depth = 0;
	size_t object_start = std::string::npos;
	for (size_t idx = 0; idx < array_text.size(); ++idx) {
		char ch = array_text[idx];
		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				in_string = false;
			}
			continue;
		}

		if (ch == '"') {
			in_string = true;
			continue;
		}
		if (ch == '{') {
			if (object_depth == 0)
				object_start = idx;
			++object_depth;
		} else if (ch == '}') {
			if (object_depth == 0)
				return false;
			--object_depth;
			if (object_depth == 0 && object_start != std::string::npos) {
				objects.push_back(array_text.substr(object_start, idx - object_start + 1));
				object_start = std::string::npos;
			}
		}
	}

	return !objects.empty();
}

static bool populate_quad_from_json_frame(const std::string &frame_json, float sx, float sy, QuadFrame &quad)
{
	bool have_flat_keys = true;
	for (int point = 0; point < 4; ++point) {
		std::string key_x = "x" + std::to_string(point);
		std::string key_y = "y" + std::to_string(point);
		double x = 0.0;
		double y = 0.0;
		if (!json_try_get_number(frame_json, key_x.c_str(), x) || !json_try_get_number(frame_json, key_y.c_str(), y)) {
			have_flat_keys = false;
			break;
		}
		quad.points[(size_t)point].x = (float)x * sx;
		quad.points[(size_t)point].y = (float)y * sy;
	}
	if (have_flat_keys)
		return true;

	std::string quad_text;
	if (!json_try_get_array_text(frame_json, "quad", quad_text))
		return false;

	std::vector<double> quad_values;
	json_collect_numbers(quad_text, quad_values);
	if (quad_values.size() != 8)
		return false;

	for (size_t point = 0; point < 4; ++point) {
		quad.points[point].x = (float)quad_values[point * 2] * sx;
		quad.points[point].y = (float)quad_values[point * 2 + 1] * sy;
	}
	return true;
}

static bool load_track_frames_from_json(const std::string &path, uint32_t output_width, uint32_t output_height,
					std::vector<QuadFrame> &frames_out, bool &any_valid, std::string &error)
{
	std::string json_text;
	if (!read_text_file_utf8(path, json_text)) {
		error = "failed to read track JSON";
		return false;
	}

	double src_w = (double)output_width;
	double src_h = (double)output_height;
	json_try_get_number(json_text, "width", src_w);
	json_try_get_number(json_text, "height", src_h);

	std::string frames_text;
	if (!json_try_get_array_text(json_text, "frames", frames_text)) {
		error = "track JSON does not contain frames";
		return false;
	}

	std::vector<std::string> frame_objects;
	if (!json_split_top_level_objects(frames_text, frame_objects)) {
		error = "track JSON does not contain any frames";
		return false;
	}

	frames_out.clear();
	frames_out.reserve(frame_objects.size());
	float sx = (float)output_width / (float)std::max(1.0, src_w);
	float sy = (float)output_height / (float)std::max(1.0, src_h);
	any_valid = false;

	for (const auto &frame_text : frame_objects) {
		QuadFrame quad {};
		bool valid = false;
		if (json_try_get_bool(frame_text, "valid", valid))
			quad.valid = valid;
		if (!populate_quad_from_json_frame(frame_text, sx, sy, quad))
			continue;
		prepare_quad_frame(quad, output_width, output_height);

		frames_out.push_back(quad);
		any_valid = any_valid || quad.valid;
	}

	if (frames_out.empty()) {
		error = "track JSON did not produce any usable frames";
		return false;
	}
	return true;
}

static bool load_track_frames_from_path(const std::string &path, uint32_t output_width, uint32_t output_height,
					std::vector<QuadFrame> &frames_out, bool &any_valid, std::string &error)
{
	if (is_npz_path(path.c_str())) {
		if (load_track_frames_from_npz(path, output_width, output_height, frames_out, any_valid, error))
			return true;

		std::string npz_error = error;
		std::string fallback_json = resolve_track_json_fallback_candidate(path);
		if (!fallback_json.empty() && fallback_json != path) {
			std::string fallback_error;
			if (load_track_frames_from_json(fallback_json, output_width, output_height, frames_out, any_valid, fallback_error)) {
				obs_log(LOG_WARNING, "falling back to sibling JSON track for unsupported NPZ '%s': %s", path.c_str(),
					npz_error.c_str());
				return true;
			}
		}

		error = npz_error;
		return false;
	}

	return load_track_frames_from_json(path, output_width, output_height, frames_out, any_valid, error);
}

static double read_audio_sample_normalized(const uint8_t *data, enum audio_format format)
{
	if (!data)
		return 0.0;

	switch (format) {
	case AUDIO_FORMAT_U8BIT:
	case AUDIO_FORMAT_U8BIT_PLANAR:
		return ((double)data[0] - 128.0) / 128.0;

	case AUDIO_FORMAT_16BIT:
	case AUDIO_FORMAT_16BIT_PLANAR:
		return (double)(*reinterpret_cast<const int16_t *>(data)) / 32768.0;

	case AUDIO_FORMAT_32BIT:
	case AUDIO_FORMAT_32BIT_PLANAR:
		return (double)(*reinterpret_cast<const int32_t *>(data)) / 2147483648.0;

	case AUDIO_FORMAT_FLOAT:
	case AUDIO_FORMAT_FLOAT_PLANAR:
		return (double)(*reinterpret_cast<const float *>(data));

	case AUDIO_FORMAT_UNKNOWN:
		return 0.0;
	}

	return 0.0;
}

static bool compute_audio_metrics_from_obs_audio(const struct audio_data *audio, enum audio_format format,
						 enum speaker_layout speakers, float &out_rms, float &out_zcr)
{
	out_rms = 0.0f;
	out_zcr = 0.0f;
	if (!audio || audio->frames == 0)
		return false;

	const uint32_t channels = get_audio_channels(speakers);
	const size_t bytes_per_channel = get_audio_bytes_per_channel(format);
	if (channels == 0 || bytes_per_channel == 0)
		return false;

	double sum_sq = 0.0;
	size_t zero_crossings = 0;
	double prev = 0.0;
	bool have_prev = false;
	size_t frame_count = 0;
	const bool planar = is_audio_planar(format);

	if (planar) {
		const size_t planes = std::min<size_t>(channels, get_audio_planes(format, speakers));
		for (uint32_t frame = 0; frame < audio->frames; ++frame) {
			double mono = 0.0;
			size_t used_channels = 0;
			for (size_t plane = 0; plane < planes; ++plane) {
				if (!audio->data[plane])
					continue;
				mono += read_audio_sample_normalized(audio->data[plane] + ((size_t)frame * bytes_per_channel), format);
				++used_channels;
			}
			if (used_channels == 0)
				continue;
			mono /= (double)used_channels;
			sum_sq += mono * mono;
			if (have_prev && ((prev < 0.0 && mono >= 0.0) || (prev >= 0.0 && mono < 0.0)))
				++zero_crossings;
			prev = mono;
			have_prev = true;
			++frame_count;
		}
	} else {
		const uint8_t *interleaved = audio->data[0];
		if (!interleaved)
			return false;
		for (uint32_t frame = 0; frame < audio->frames; ++frame) {
			double mono = 0.0;
			for (uint32_t channel = 0; channel < channels; ++channel) {
				const size_t offset = (((size_t)frame * channels) + (size_t)channel) * bytes_per_channel;
				mono += read_audio_sample_normalized(interleaved + offset, format);
			}
			mono /= (double)channels;
			sum_sq += mono * mono;
			if (have_prev && ((prev < 0.0 && mono >= 0.0) || (prev >= 0.0 && mono < 0.0)))
				++zero_crossings;
			prev = mono;
			have_prev = true;
			++frame_count;
		}
	}

	if (frame_count == 0)
		return false;

	out_rms = (float)sqrt(sum_sq / (double)frame_count);
	out_zcr = frame_count > 1 ? (float)zero_crossings / (float)(frame_count - 1) : 0.0f;
	return true;
}

class NativeRuntime {
public:
	explicit NativeRuntime(const struct mpt_native_runtime_config *config)
		: render_fps_(config ? (int)config->render_fps : 30),
		  audio_device_index_(config ? config->audio_device_index : -1),
		  valid_policy_(config && has_text(config->valid_policy) ? config->valid_policy : "hold"),
		  loop_video_path_(config && config->loop_video ? config->loop_video : ""),
		  mouth_dir_path_(config && config->mouth_dir ? config->mouth_dir : ""),
		  track_file_path_(config && config->track_file ? config->track_file : ""),
		  track_calibrated_path_(config && config->track_calibrated_file ? config->track_calibrated_file : ""),
		  obs_audio_source_uuid_(config && config->audio_sync_source_uuid ? config->audio_sync_source_uuid : ""),
		  audio_identity_json_(config && config->audio_device_identity_json ? config->audio_device_identity_json : "")
	{
		if (render_fps_ <= 0)
			render_fps_ = 30;
	}

	~NativeRuntime()
	{
		shutdown_audio();
		mpt_image_backend_destroy(image_backend_);
		mpt_video_backend_destroy(video_backend_);
		if (co_initialized_)
			CoUninitialize();
	}

	bool initialize(std::string &error)
	{
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (SUCCEEDED(hr))
			co_initialized_ = true;
		else if (hr != RPC_E_CHANGED_MODE) {
			error = "CoInitializeEx failed";
			return false;
		}

		if (!mpt_image_backend_create(&image_backend_, error)) {
			return false;
		}
		if (!mpt_video_backend_create(&video_backend_, error))
			return false;

		if (!initialize_video(error))
			return false;
		if (!initialize_sprites(error))
			return false;
		if (!initialize_track(error))
			return false;
		initialize_audio();
		return true;
	}

	bool render_frame(uint8_t **out_bgra, size_t *out_size, uint32_t *out_width, uint32_t *out_height, uint32_t *out_stride,
			  uint64_t *out_timestamp)
	{
		if (!out_bgra || !out_size || !out_width || !out_height || !out_stride || !out_timestamp)
			return false;
		uint64_t video_timestamp_ns = 0;
		if (!read_next_video_frame(output_, video_timestamp_ns))
			return false;

		uint64_t frame_interval_ns = 1000000000ULL / (uint64_t)std::max(render_fps_, 1);
		uint64_t output_timestamp_ns =
			next_output_timestamp_ns_ == 0 ? os_gettime_ns() : (next_output_timestamp_ns_ + frame_interval_ns);
		update_mouth_state(output_timestamp_ns);
		const ImageBGRA &sprite = current_sprite();
		const QuadFrame *quad = current_track_frame();
		if (!sprite.empty() && quad)
			warp_and_blend(output_, sprite, *quad);

		++frame_index_;
		next_output_timestamp_ns_ = output_timestamp_ns;
		*out_bgra = output_.pixels.data();
		*out_size = output_.pixels.size();
		*out_width = output_.width;
		*out_height = output_.height;
		*out_stride = (uint32_t)output_.stride();
		*out_timestamp = next_output_timestamp_ns_;
		return true;
	}

	void get_dimensions(uint32_t *out_width, uint32_t *out_height) const
	{
		if (out_width)
			*out_width = output_.width;
		if (out_height)
			*out_height = output_.height;
	}

private:
	bool initialize_video(std::string &error)
	{
		return mpt_video_backend_open_loop_video(video_backend_, loop_video_path_, &output_, error);
	}

	bool initialize_sprites(std::string &error)
	{
		if (mouth_dir_path_.empty()) {
			error = "Mouth directory is required.";
			return false;
		}

		std::filesystem::path dir = resolve_sprite_directory(error);
		if (dir.empty())
			return false;

		ImageBGRA open_image = load_sprite(dir / "open.png", error);
		if (open_image.empty())
			return false;
		open_image = crop_to_alpha(open_image, output_.width, output_.height);

		sprites_[0] = load_optional_sprite(dir / "closed.png", open_image);
		sprites_[1] = load_optional_sprite(dir / "half.png", open_image);
		sprites_[2] = open_image;
		sprites_[3] = load_optional_sprite(dir / "u.png", open_image);
		sprites_[4] = load_optional_sprite(dir / "e.png", open_image);
		return true;
	}

	bool initialize_track(std::string &error)
	{
		std::string selected_track = select_track_path_from_state(error);
		if (selected_track.empty())
			return false;
		bool any_valid = false;
		if (!load_track_frames_from_path(selected_track, output_.width, output_.height, track_frames_, any_valid, error))
			return false;

		track_filled_ = track_frames_;
		if (any_valid) {
			size_t first_valid = 0;
			while (first_valid < track_frames_.size() && !track_frames_[first_valid].valid)
				++first_valid;
			if (first_valid < track_frames_.size()) {
				size_t last_valid = first_valid;
				for (size_t idx = 0; idx < track_frames_.size(); ++idx) {
					if (track_frames_[idx].valid) {
						last_valid = idx;
					} else {
						track_filled_[idx] = track_filled_[last_valid];
					}
				}
				for (size_t idx = 0; idx < first_valid; ++idx)
					track_filled_[idx] = track_filled_[first_valid];
			}
		}
		track_has_any_valid_ = any_valid;
		return true;
	}

	std::filesystem::path resolve_sprite_directory(std::string &error) const
	{
		std::filesystem::path base = utf8_to_path(mouth_dir_path_);
		if (!std::filesystem::exists(base)) {
			error = "mouth directory was not found";
			return std::filesystem::path();
		}
		if (std::filesystem::is_regular_file(base / "open.png"))
			return base;

		std::vector<std::filesystem::path> candidates;
		for (const auto &entry : std::filesystem::directory_iterator(base)) {
			if (!entry.is_directory())
				continue;
			if (std::filesystem::is_regular_file(entry.path() / "open.png"))
				candidates.push_back(entry.path());
		}

		if (candidates.empty()) {
			error = "mouth directory does not contain open.png";
			return std::filesystem::path();
		}

		auto pick_named = [&](const char *name) -> std::filesystem::path {
			std::string lower = to_lower_ascii(name);
			for (const auto &candidate : candidates) {
				if (to_lower_ascii(candidate.filename().u8string()) == lower)
					return candidate;
			}
			return std::filesystem::path();
		};

		std::filesystem::path preferred = pick_named("neutral");
		if (!preferred.empty())
			return preferred;
		preferred = pick_named("default");
		if (!preferred.empty())
			return preferred;
		return candidates.front();
	}

	ImageBGRA load_sprite(const std::filesystem::path &path, std::string &error) const
	{
		return mpt_image_backend_load_png_bgra(image_backend_, path, error);
	}

	ImageBGRA load_optional_sprite(const std::filesystem::path &path, const ImageBGRA &fallback) const
	{
		if (std::filesystem::exists(path)) {
			std::string ignored_error;
			ImageBGRA loaded = load_sprite(path, ignored_error);
			if (!loaded.empty())
				return crop_to_alpha(loaded, output_.width, output_.height);
		}

		const char *key = "open";
		if (path.filename() == "closed.png")
			key = "closed";
		else if (path.filename() == "half.png")
			key = "half";
		else if (path.filename() == "u.png")
			key = "u";
		else if (path.filename() == "e.png")
			key = "e";
		return make_variant_from_open(fallback, key);
	}

	std::string select_track_path_from_state(std::string &error) const
	{
		std::string track = resolve_track_input_candidate(track_calibrated_path_);
		if (!track.empty())
			return track;
		track = resolve_track_input_candidate(track_file_path_);
		if (!track.empty())
			return track;

		error = "Track file must be a JSON export or a supported NPZ archive.";
		return std::string();
	}

	bool read_next_video_frame(ImageBGRA &image, uint64_t &timestamp_ns)
	{
		return mpt_video_backend_read_next_frame(video_backend_, image, timestamp_ns);
	}

	bool initialize_obs_audio_format()
	{
		audio_t *obs_audio = obs_get_audio();
		const struct audio_output_info *info = obs_audio ? audio_output_get_info(obs_audio) : nullptr;
		if (!info || info->samples_per_sec == 0)
			return false;
		if (get_audio_channels(info->speakers) == 0 || get_audio_bytes_per_channel(info->format) == 0)
			return false;

		obs_audio_sample_rate_ = info->samples_per_sec;
		obs_audio_format_ = info->format;
		obs_audio_speakers_ = info->speakers;
		return true;
	}

	void clear_audio_analysis_windows()
	{
		std::lock_guard<std::mutex> lock(audio_windows_mutex_);
		audio_windows_.clear();
	}

	void queue_audio_analysis_window(uint64_t start_timestamp_ns, uint64_t end_timestamp_ns, float rms, float zcr)
	{
		latest_rms_.store(rms);
		latest_zcr_.store(zcr);
		if (end_timestamp_ns <= start_timestamp_ns)
			return;

		std::lock_guard<std::mutex> lock(audio_windows_mutex_);
		if (!audio_windows_.empty() && start_timestamp_ns < audio_windows_.back().start_timestamp_ns)
			audio_windows_.clear();

		audio_windows_.push_back({start_timestamp_ns, end_timestamp_ns, rms, zcr});
		uint64_t keep_after_ns = end_timestamp_ns > 3000000000ULL ? end_timestamp_ns - 3000000000ULL : 0ULL;
		while (!audio_windows_.empty() && audio_windows_.front().end_timestamp_ns < keep_after_ns)
			audio_windows_.pop_front();
		while (audio_windows_.size() > 256)
			audio_windows_.pop_front();
	}

	bool find_audio_metrics_for_timestamp(uint64_t timestamp_ns, float &out_rms, float &out_zcr)
	{
		std::lock_guard<std::mutex> lock(audio_windows_mutex_);
		if (audio_windows_.empty())
			return false;

		while (audio_windows_.size() > 1 && audio_windows_[1].end_timestamp_ns <= timestamp_ns)
			audio_windows_.pop_front();

		const AudioAnalysisWindow *selected = nullptr;
		for (const auto &window : audio_windows_) {
			if (window.start_timestamp_ns <= timestamp_ns && timestamp_ns < window.end_timestamp_ns) {
				selected = &window;
				break;
			}
			if (window.end_timestamp_ns <= timestamp_ns)
				selected = &window;
			else
				break;
		}
		if (!selected)
			return false;
		if (timestamp_ns > selected->end_timestamp_ns + 250000000ULL)
			return false;

		out_rms = selected->rms;
		out_zcr = selected->zcr;
		return true;
	}

	bool obs_audio_follow_requested() const
	{
		return !obs_audio_source_uuid_.empty();
	}

	void reset_audio_analysis_state()
	{
		clear_audio_analysis_windows();
		latest_rms_.store(0.0f);
		latest_zcr_.store(0.0f);
		noise_floor_ = 0.0001f;
		peak_level_ = 0.001f;
		env_lp_ = 0.0f;
		mouth_shape_index_ = 0;
	}

	void stop_direct_input_capture()
	{
		if (!audio_capture_)
			return;
		mpt_audio_backend_stop_input_capture(audio_capture_);
		audio_capture_ = nullptr;
	}

	void ensure_direct_input_capture_started(bool log_failure)
	{
		if (audio_capture_)
			return;

		std::string error;
		if (mpt_audio_backend_start_input_capture(audio_identity_json_, audio_device_index_,
							  &NativeRuntime::audio_input_callback, this, &audio_capture_, error)) {
			audio_capture_warning_logged_ = false;
			return;
		}

		if (!audio_capture_warning_logged_ || log_failure) {
			obs_log(LOG_WARNING, "failed to start fallback direct input capture for MotionPngTuberPlayer: %s",
				error.empty() ? "unknown error" : error.c_str());
			audio_capture_warning_logged_ = true;
		}
	}

	bool try_attach_obs_audio_source(bool log_if_missing)
	{
		if (!obs_audio_follow_requested())
			return false;
		if (obs_audio_source_) {
			stop_direct_input_capture();
			return true;
		}

		uint64_t now_ns = os_gettime_ns();
		if (now_ns < next_obs_audio_attach_attempt_ns_)
			return false;
		next_obs_audio_attach_attempt_ns_ = now_ns + 1000000000ULL;

		if (!initialize_obs_audio_format()) {
			if (!obs_audio_attach_warning_logged_ || log_if_missing) {
				obs_log(LOG_WARNING, "failed to initialize OBS audio format for MotionPngTuberPlayer lip sync follow mode");
				obs_audio_attach_warning_logged_ = true;
			}
			return false;
		}

		obs_source_t *source = obs_get_source_by_uuid(obs_audio_source_uuid_.c_str());
		if (!source) {
			if (!obs_audio_attach_warning_logged_ || log_if_missing) {
				obs_log(LOG_WARNING, "OBS audio source for MotionPngTuberPlayer lip sync was not found: %s",
					obs_audio_source_uuid_.c_str());
				obs_audio_attach_warning_logged_ = true;
			}
			return false;
		}
		if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0) {
			const char *name = obs_source_get_name(source);
			obs_log(LOG_WARNING, "OBS source '%s' does not output audio and cannot drive MotionPngTuberPlayer lip sync",
				name && *name ? name : obs_audio_source_uuid_.c_str());
			obs_source_release(source);
			obs_audio_attach_warning_logged_ = true;
			return false;
		}

		stop_direct_input_capture();
		obs_source_add_audio_capture_callback(source, &NativeRuntime::obs_audio_capture_callback, this);
		obs_audio_source_ = source;
		obs_audio_attach_warning_logged_ = false;
		reset_audio_analysis_state();
		return true;
	}

	void handle_obs_source_audio(const struct audio_data *audio, bool muted)
	{
		if (!audio || audio->frames == 0 || obs_audio_sample_rate_ == 0) {
			latest_rms_.store(0.0f);
			latest_zcr_.store(0.0f);
			return;
		}

		float rms = 0.0f;
		float zcr = 0.0f;
		if (!muted)
			compute_audio_metrics_from_obs_audio(audio, obs_audio_format_, obs_audio_speakers_, rms, zcr);

		uint64_t duration_ns = audio_frames_to_ns(obs_audio_sample_rate_, audio->frames);
		queue_audio_analysis_window(audio->timestamp, audio->timestamp + duration_ns, rms, zcr);
	}

	void initialize_audio()
	{
		if (obs_audio_follow_requested() && try_attach_obs_audio_source(true))
			return;

		ensure_direct_input_capture_started(true);
	}

	void shutdown_audio()
	{
		{
			std::lock_guard<std::mutex> lock(obs_audio_callback_mutex_);
			obs_audio_callback_shutdown_ = true;
		}
		if (obs_audio_source_) {
			obs_source_remove_audio_capture_callback(obs_audio_source_, &NativeRuntime::obs_audio_capture_callback, this);
			obs_source_release(obs_audio_source_);
			obs_audio_source_ = nullptr;
		}
		{
			std::unique_lock<std::mutex> lock(obs_audio_callback_mutex_);
			obs_audio_callback_cv_.wait(lock, [this] { return obs_audio_callbacks_in_flight_ == 0; });
		}
		reset_audio_analysis_state();
		next_obs_audio_attach_attempt_ns_ = 0;

		stop_direct_input_capture();
	}

	static void obs_audio_capture_callback(void *param, obs_source_t *source, const struct audio_data *audio_data,
					     bool muted)
	{
		UNUSED_PARAMETER(source);
		auto *runtime = reinterpret_cast<NativeRuntime *>(param);
		if (!runtime || !runtime->begin_obs_audio_callback())
			return;
		runtime->handle_obs_source_audio(audio_data, muted);
		runtime->end_obs_audio_callback();
	}

	static void audio_input_callback(const int16_t *samples, size_t sample_count, uint16_t channels, uint32_t sample_rate,
					 void *userdata)
	{
		auto *runtime = reinterpret_cast<NativeRuntime *>(userdata);
		if (!runtime)
			return;
		runtime->handle_audio_samples(samples, sample_count, channels, sample_rate);
	}

	void handle_audio_samples(const int16_t *samples, size_t sample_count, uint16_t channels, uint32_t sample_rate)
	{
		UNUSED_PARAMETER(sample_rate);
		if (!samples || sample_count == 0)
			return;

		double sum_sq = 0.0;
		size_t zero_crossings = 0;
		double prev = 0.0;
		bool have_prev = false;
		uint16_t channel_step = std::max<uint16_t>(1, channels);
		for (size_t idx = 0; idx < sample_count; idx += channel_step) {
			double mono = 0.0;
			uint16_t used_channels = 0;
			for (uint16_t channel = 0; channel < channel_step && idx + channel < sample_count; ++channel) {
				mono += (double)samples[idx + channel] / 32768.0;
				++used_channels;
			}
			if (used_channels == 0)
				continue;

			mono /= (double)used_channels;
			sum_sq += mono * mono;
			if (have_prev && ((prev < 0.0 && mono >= 0.0) || (prev >= 0.0 && mono < 0.0)))
				++zero_crossings;
			prev = mono;
			have_prev = true;
		}

		size_t mono_count = sample_count / channel_step;
		float rms = mono_count > 0 ? (float)sqrt(sum_sq / (double)mono_count) : 0.0f;
		float zcr = mono_count > 1 ? (float)zero_crossings / (float)(mono_count - 1) : 0.0f;
		latest_rms_.store(rms);
		latest_zcr_.store(zcr);
	}

	void apply_mouth_metrics(float rms, float zcr, bool enable_temporal_smoothing)
	{
		if (rms < noise_floor_ + 0.0005f)
			noise_floor_ = noise_floor_ * 0.995f + rms * 0.005f;
		else
			noise_floor_ = noise_floor_ * 0.999f + rms * 0.001f;

		peak_level_ = std::max(rms, peak_level_ * 0.995f);
		float denom = std::max(peak_level_ - noise_floor_, 0.01f);
		float normalized = std::clamp((rms - noise_floor_) / denom, 0.0f, 1.0f);
		normalized = sqrtf(normalized);

		float env = normalized;
		if (enable_temporal_smoothing) {
			env_lp_ += 0.35f * (normalized - env_lp_);
			env = std::clamp(env_lp_ * 0.75f + normalized * 0.25f, 0.0f, 1.0f);
		} else {
			env_lp_ = normalized;
		}

		if (env < 0.08f) {
			mouth_shape_index_ = 0;
			return;
		}
		if (env < 0.32f) {
			mouth_shape_index_ = 1;
			return;
		}
		if (zcr < 0.10f)
			mouth_shape_index_ = 3;
		else if (zcr > 0.22f)
			mouth_shape_index_ = 4;
		else
			mouth_shape_index_ = 2;
	}

	void update_mouth_state(uint64_t output_timestamp_ns)
	{
		if (obs_audio_follow_requested()) {
			float rms = 0.0f;
			float zcr = 0.0f;
			if (try_attach_obs_audio_source(false) && find_audio_metrics_for_timestamp(output_timestamp_ns, rms, zcr))
				apply_mouth_metrics(rms, zcr, false);
			else {
				ensure_direct_input_capture_started(false);
				apply_mouth_metrics(latest_rms_.load(), latest_zcr_.load(), true);
			}
			return;
		}

		apply_mouth_metrics(latest_rms_.load(), latest_zcr_.load(), true);
	}

	const ImageBGRA &current_sprite() const
	{
		return sprites_[std::clamp(mouth_shape_index_, 0, 4)];
	}

	const QuadFrame *current_track_frame() const
	{
		if (track_frames_.empty() || !track_has_any_valid_)
			return nullptr;

		size_t index = frame_index_ % track_frames_.size();
		if (valid_policy_ == "strict") {
			if (!track_frames_[index].valid || !track_frames_[index].warp_ready)
				return nullptr;
			return &track_frames_[index];
		}
		if (!track_filled_[index].warp_ready)
			return nullptr;
		return &track_filled_[index];
	}

	bool begin_obs_audio_callback()
	{
		std::lock_guard<std::mutex> lock(obs_audio_callback_mutex_);
		if (obs_audio_callback_shutdown_)
			return false;
		++obs_audio_callbacks_in_flight_;
		return true;
	}

	void end_obs_audio_callback()
	{
		std::lock_guard<std::mutex> lock(obs_audio_callback_mutex_);
		if (obs_audio_callbacks_in_flight_ > 0)
			--obs_audio_callbacks_in_flight_;
		if (obs_audio_callback_shutdown_ && obs_audio_callbacks_in_flight_ == 0)
			obs_audio_callback_cv_.notify_all();
	}

private:
	bool co_initialized_ = false;
	MptImageBackend *image_backend_ = nullptr;
	MptVideoBackend *video_backend_ = nullptr;
	ImageBGRA output_ {};
	std::array<ImageBGRA, 5> sprites_ {};
	std::vector<QuadFrame> track_frames_;
	std::vector<QuadFrame> track_filled_;
	bool track_has_any_valid_ = false;
	size_t frame_index_ = 0;
	int render_fps_ = 30;
	uint64_t next_output_timestamp_ns_ = 0;
	long long audio_device_index_ = -1;
	std::string valid_policy_;
	std::string loop_video_path_;
	std::string mouth_dir_path_;
	std::string track_file_path_;
	std::string track_calibrated_path_;
	std::string obs_audio_source_uuid_;
	std::string audio_identity_json_;
	MptAudioCapture *audio_capture_ = nullptr;
	obs_source_t *obs_audio_source_ = nullptr;
	uint32_t obs_audio_sample_rate_ = 0;
	enum audio_format obs_audio_format_ = AUDIO_FORMAT_UNKNOWN;
	enum speaker_layout obs_audio_speakers_ = SPEAKERS_UNKNOWN;
	uint64_t next_obs_audio_attach_attempt_ns_ = 0;
	bool obs_audio_attach_warning_logged_ = false;
	bool audio_capture_warning_logged_ = false;
	std::mutex obs_audio_callback_mutex_;
	std::condition_variable obs_audio_callback_cv_;
	uint32_t obs_audio_callbacks_in_flight_ = 0;
	bool obs_audio_callback_shutdown_ = false;
	std::mutex audio_windows_mutex_;
	std::deque<AudioAnalysisWindow> audio_windows_;
	std::atomic<float> latest_rms_ {0.0f};
	std::atomic<float> latest_zcr_ {0.0f};
	float noise_floor_ = 0.0001f;
	float peak_level_ = 0.001f;
	float env_lp_ = 0.0f;
	int mouth_shape_index_ = 0;
};

static bool enum_obs_audio_source_for_list(void *param, obs_source_t *source)
{
	auto *list = reinterpret_cast<obs_property_t *>(param);
	if (!list || !source)
		return true;
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
		return true;

	const char *uuid = obs_source_get_uuid(source);
	if (!has_text(uuid))
		return true;

	const char *name = obs_source_get_name(source);
	obs_property_list_add_string(list, has_text(name) ? name : uuid, uuid);
	return true;
}

} // namespace

extern "C" void mpt_native_populate_audio_devices(obs_property_t *list)
{
	if (!list)
		return;

	obs_property_list_clear(list);
	obs_property_list_add_string(list, mpt_text("MotionPngTuberPlayer.AudioDeviceAuto"), "");

	std::vector<MptAudioInputDevice> devices = mpt_audio_backend_enumerate_input_devices();
	if (devices.empty()) {
		add_disabled_list_item(list, mpt_text("MotionPngTuberPlayer.AudioDeviceNone"));
		return;
	}

	for (auto &device : devices) {
		if (device.name.empty())
			device.name = mpt_text("MotionPngTuberPlayer.AudioDeviceUnnamed");
		if (device.label.empty())
			device.label = std::to_string(device.index) + " " + device.name;
		obs_property_list_add_string(list, device.label.c_str(), device.identity_json.c_str());
	}
}

extern "C" void mpt_native_populate_obs_audio_sources(obs_property_t *list)
{
	if (!list)
		return;

	obs_property_list_clear(list);
	obs_property_list_add_string(list, mpt_text("MotionPngTuberPlayer.AudioSyncSourceAuto"),
				     kAutoObsAudioSourceSelection);
	obs_property_list_add_string(list, mpt_text("MotionPngTuberPlayer.AudioSyncSourceNone"),
				     kDirectInputAudioSourceSelection);
	obs_enum_sources(&enum_obs_audio_source_for_list, list);
}

extern "C" bool mpt_native_runtime_create(struct mpt_native_runtime **out_runtime,
					 const struct mpt_native_runtime_config *config, char **error_text)
{
	if (error_text)
		*error_text = nullptr;
	if (!out_runtime) {
		if (error_text)
			*error_text = dup_error("invalid native runtime output pointer");
		return false;
	}

	*out_runtime = nullptr;
	auto *wrapper = new (std::nothrow) mpt_native_runtime();
	auto *impl = new (std::nothrow) NativeRuntime(config);
	if (!wrapper || !impl) {
		delete wrapper;
		delete impl;
		if (error_text)
			*error_text = dup_error("out of memory while creating native runtime");
		return false;
	}

	std::string error;
	if (!impl->initialize(error)) {
		delete impl;
		delete wrapper;
		if (error_text)
			*error_text = dup_error(error);
		return false;
	}

	wrapper->impl = impl;
	*out_runtime = wrapper;
	return true;
}

extern "C" void mpt_native_runtime_destroy(struct mpt_native_runtime *runtime)
{
	if (!runtime)
		return;
	delete reinterpret_cast<NativeRuntime *>(runtime->impl);
	delete runtime;
}

extern "C" bool mpt_native_runtime_render_frame(struct mpt_native_runtime *runtime, uint8_t **out_bgra, size_t *out_size,
						 uint32_t *out_width, uint32_t *out_height, uint32_t *out_stride,
						 uint64_t *out_timestamp)
{
	if (!runtime || !runtime->impl)
		return false;
	return reinterpret_cast<NativeRuntime *>(runtime->impl)
		->render_frame(out_bgra, out_size, out_width, out_height, out_stride, out_timestamp);
}

extern "C" void mpt_native_runtime_get_dimensions(struct mpt_native_runtime *runtime, uint32_t *out_width,
						   uint32_t *out_height)
{
	if (!runtime || !runtime->impl)
		return;
	reinterpret_cast<NativeRuntime *>(runtime->impl)->get_dimensions(out_width, out_height);
}
