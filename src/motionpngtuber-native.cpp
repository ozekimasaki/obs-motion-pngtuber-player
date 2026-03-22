#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "motionpngtuber-native.h"
#include "mpt-audio-backend.h"
#include "mpt-image-backend.h"
#include "mpt-text.h"
#include "mpt-video-backend.h"

#ifndef MPT_FALLBACK_OBS
#include <util/platform.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <mmeapi.h>
#include <objbase.h>
#endif

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

#ifdef _WIN32
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

static std::filesystem::path utf8_to_path(const char *text)
{
	if (!text || !*text)
		return std::filesystem::path();
#ifdef _WIN32
	return std::filesystem::path(utf8_to_wide(text));
#else
	return std::filesystem::u8path(text);
#endif
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

static std::string resolve_track_json_candidate(const std::string &path)
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
#ifdef _WIN32
		if (co_initialized_)
			CoUninitialize();
#endif
	}

	bool initialize(std::string &error)
	{
#ifdef _WIN32
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (SUCCEEDED(hr))
			co_initialized_ = true;
		else if (hr != RPC_E_CHANGED_MODE) {
			error = "CoInitializeEx failed";
			return false;
		}
#endif

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

		update_mouth_state();
		const ImageBGRA &sprite = current_sprite();
		const QuadFrame *quad = current_track_frame();
		if (!sprite.empty() && quad)
			warp_and_blend(output_, sprite, *quad);

		++frame_index_;
		uint64_t frame_interval_ns = 1000000000ULL / (uint64_t)std::max(render_fps_, 1);
		if (next_output_timestamp_ns_ == 0)
			next_output_timestamp_ns_ = os_gettime_ns();
		else
			next_output_timestamp_ns_ += frame_interval_ns;
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
		std::string selected_track = select_track_json_path_from_state(error);
		if (selected_track.empty())
			return false;
		bool any_valid = false;
		if (!load_track_frames_from_json(selected_track, output_.width, output_.height, track_frames_, any_valid, error))
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

	std::string select_track_json_path_from_state(std::string &error) const
	{
		std::string track = resolve_track_json_candidate(track_calibrated_path_);
		if (!track.empty())
			return track;
		track = resolve_track_json_candidate(track_file_path_);
		if (!track.empty())
			return track;

		error =
			"Track file must be a JSON export or an NPZ with a sibling mouth_track.json generated by convert_npz_to_json.py.";
		return std::string();
	}

	bool read_next_video_frame(ImageBGRA &image, uint64_t &timestamp_ns)
	{
		return mpt_video_backend_read_next_frame(video_backend_, image, timestamp_ns);
	}

	void initialize_audio()
	{
#ifdef _WIN32
		UINT device_index = resolve_audio_device();
		if (device_index == UINT_MAX)
			return;

		struct FormatCandidate {
			uint16_t channels;
			uint32_t sample_rate;
		};
		const FormatCandidate candidates[] = {
			{1, 44100},
			{1, 48000},
			{2, 44100},
			{2, 48000},
		};

		for (const auto &candidate : candidates) {
			HWAVEIN opened_wave_in = nullptr;
			WAVEFORMATEX wfx = {};
			wfx.wFormatTag = WAVE_FORMAT_PCM;
			wfx.nChannels = candidate.channels;
			wfx.nSamplesPerSec = candidate.sample_rate;
			wfx.wBitsPerSample = 16;
			wfx.nBlockAlign = (WORD)(wfx.nChannels * (wfx.wBitsPerSample / 8));
			wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

			if (waveInOpen(&opened_wave_in, device_index, &wfx, (DWORD_PTR)&NativeRuntime::wave_in_callback,
				       (DWORD_PTR)this, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
				continue;
			}
			wave_in_.store(opened_wave_in);

			audio_channels_ = wfx.nChannels;
			audio_sample_rate_ = wfx.nSamplesPerSec;
			size_t samples_per_buffer = 1024U;
			for (size_t idx = 0; idx < audio_headers_.size(); ++idx) {
				audio_storage_[idx].resize(samples_per_buffer * audio_channels_);
				memset(&audio_headers_[idx], 0, sizeof(WAVEHDR));
				audio_headers_[idx].lpData = (LPSTR)audio_storage_[idx].data();
				audio_headers_[idx].dwBufferLength =
					(DWORD)(audio_storage_[idx].size() * sizeof(int16_t));
				if (waveInPrepareHeader(opened_wave_in, &audio_headers_[idx], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
					shutdown_audio();
					return;
				}
				if (waveInAddBuffer(opened_wave_in, &audio_headers_[idx], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
					shutdown_audio();
					return;
				}
			}
			if (waveInStart(opened_wave_in) == MMSYSERR_NOERROR)
				return;
			shutdown_audio();
		}
#endif
	}

	void shutdown_audio()
	{
#ifdef _WIN32
		HWAVEIN wave_in = wave_in_.exchange(nullptr);
		if (!wave_in)
			return;
		waveInStop(wave_in);
		waveInReset(wave_in);
		for (auto &header : audio_headers_) {
			if (header.dwFlags & WHDR_PREPARED)
				waveInUnprepareHeader(wave_in, &header, sizeof(WAVEHDR));
		}
		waveInClose(wave_in);
#endif
	}

#ifdef _WIN32
	UINT resolve_audio_device() const
	{
		uint32_t device_index = 0;
		if (!mpt_audio_backend_resolve_input_device(audio_identity_json_, audio_device_index_, &device_index))
			return UINT_MAX;
		return static_cast<UINT>(device_index);
	}

	static void CALLBACK wave_in_callback(HWAVEIN hwi, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2)
	{
		UNUSED_PARAMETER(hwi);
		UNUSED_PARAMETER(param2);
		if (msg != WIM_DATA || !instance || !param1)
			return;
		auto *runtime = reinterpret_cast<NativeRuntime *>(instance);
		runtime->handle_audio_buffer(reinterpret_cast<WAVEHDR *>(param1));
	}

	void handle_audio_buffer(WAVEHDR *header)
	{
		if (!header)
			return;
		auto requeue_buffer = [&](WAVEHDR *queued_header) {
			HWAVEIN wave_in = wave_in_.load();
			if (!wave_in)
				return;
			if (waveInAddBuffer(wave_in, queued_header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
				latest_rms_.store(0.0f);
				latest_zcr_.store(0.0f);
			}
		};
		if (header->dwBytesRecorded == 0) {
			requeue_buffer(header);
			return;
		}

		const int16_t *samples = reinterpret_cast<const int16_t *>(header->lpData);
		size_t sample_count = header->dwBytesRecorded / sizeof(int16_t);
		if (sample_count == 0) {
			requeue_buffer(header);
			return;
		}

		double sum_sq = 0.0;
		size_t zero_crossings = 0;
		double prev = 0.0;
		bool have_prev = false;
		for (size_t idx = 0; idx < sample_count; idx += std::max<uint16_t>(1, audio_channels_)) {
			double sample = (double)samples[idx] / 32768.0;
			sum_sq += sample * sample;
			if (have_prev && ((prev < 0.0 && sample >= 0.0) || (prev >= 0.0 && sample < 0.0)))
				++zero_crossings;
			prev = sample;
			have_prev = true;
		}

		size_t mono_count = sample_count / std::max<uint16_t>(1, audio_channels_);
		float rms = mono_count > 0 ? (float)sqrt(sum_sq / (double)mono_count) : 0.0f;
		float zcr = mono_count > 1 ? (float)zero_crossings / (float)(mono_count - 1) : 0.0f;
		latest_rms_.store(rms);
		latest_zcr_.store(zcr);

		requeue_buffer(header);
	}
#endif

	void update_mouth_state()
	{
		float rms = latest_rms_.load();
		float zcr = latest_zcr_.load();

		if (rms < noise_floor_ + 0.0005f)
			noise_floor_ = noise_floor_ * 0.995f + rms * 0.005f;
		else
			noise_floor_ = noise_floor_ * 0.999f + rms * 0.001f;

		peak_level_ = std::max(rms, peak_level_ * 0.995f);
		float denom = std::max(peak_level_ - noise_floor_, 0.01f);
		float normalized = std::clamp((rms - noise_floor_) / denom, 0.0f, 1.0f);
		normalized = sqrtf(normalized);

		env_lp_ += 0.35f * (normalized - env_lp_);
		float env = std::clamp(env_lp_ * 0.75f + normalized * 0.25f, 0.0f, 1.0f);

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
	std::string audio_identity_json_;
#ifdef _WIN32
	std::atomic<HWAVEIN> wave_in_ {nullptr};
	uint16_t audio_channels_ = 1;
	uint32_t audio_sample_rate_ = 44100;
	std::array<std::vector<int16_t>, 3> audio_storage_ {};
	std::array<WAVEHDR, 3> audio_headers_ {};
#endif
	std::atomic<float> latest_rms_ {0.0f};
	std::atomic<float> latest_zcr_ {0.0f};
	float noise_floor_ = 0.0001f;
	float peak_level_ = 0.001f;
	float env_lp_ = 0.0f;
	int mouth_shape_index_ = 0;
};

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
