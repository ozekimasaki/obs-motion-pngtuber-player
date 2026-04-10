#include "motionpngtuber-native.h"

#include <cstdarg>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <string>

extern "C" const char *mpt_text(const char *key)
{
	return key ? key : "";
}

extern "C" void obs_log(int log_level, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	blogva(log_level, format, args);
	va_end(args);
}

namespace {

struct Options {
	std::filesystem::path asset_dir;
};

std::optional<Options> parse_args(int argc, char **argv)
{
	Options options;
	for (int idx = 1; idx < argc; ++idx) {
		std::string arg = argv[idx];
		if (arg == "--asset-dir") {
			if (idx + 1 >= argc) {
				std::cerr << "missing value for --asset-dir\n";
				return std::nullopt;
			}
			options.asset_dir = argv[++idx];
		} else {
			std::cerr << "unknown argument: " << arg << '\n';
			return std::nullopt;
		}
	}

	if (options.asset_dir.empty()) {
		std::cerr << "--asset-dir is required\n";
		return std::nullopt;
	}
	return options;
}

uint64_t fnv1a_64(const uint8_t *data, size_t size)
{
	uint64_t hash = 14695981039346656037ULL;
	for (size_t idx = 0; idx < size; ++idx) {
		hash ^= static_cast<uint64_t>(data[idx]);
		hash *= 1099511628211ULL;
	}
	return hash;
}

bool expect(bool condition, const char *message)
{
	if (!condition)
		std::cerr << message << '\n';
	return condition;
}

} // namespace

int main(int argc, char **argv)
{
	auto parsed = parse_args(argc, argv);
	if (!parsed)
		return 2;
	const Options options = *parsed;

	const std::string loop_video = (options.asset_dir / "loop_motion.mp4").u8string();
	const std::string mouth_dir = (options.asset_dir / "mouth").u8string();
	const std::string track_file = (options.asset_dir / "mouth_track.npz").u8string();

	struct mpt_native_runtime_config config = {};
	config.loop_video = loop_video.c_str();
	config.mouth_dir = mouth_dir.c_str();
	config.track_file = track_file.c_str();
	config.track_calibrated_file = "";
	config.audio_device_identity_json = "";
	config.audio_sync_source_uuid = "";
	config.valid_policy = "hold";
	config.direct_input_requested = false;
	config.audio_device_index = -1;
	config.render_fps = 24;

	struct mpt_native_runtime *runtime = nullptr;
	char *error_text = nullptr;
	if (!mpt_native_runtime_create(&runtime, &config, &error_text) || !runtime) {
		std::cerr << "mpt_native_runtime_create failed: " << (error_text ? error_text : "(no error)") << '\n';
		if (error_text)
			bfree(error_text);
		return 1;
	}

	uint32_t width = 0;
	uint32_t height = 0;
	mpt_native_runtime_get_dimensions(runtime, &width, &height);
	if (!expect(width == 64 && height == 64, "native runtime dimensions should match generated Linux fixture")) {
		mpt_native_runtime_destroy(runtime);
		return 1;
	}

	std::set<uint64_t> unique_hashes;
	uint64_t previous_timestamp = 0;

	for (int frame_index = 0; frame_index < 24; ++frame_index) {
		uint8_t *bgra = nullptr;
		size_t size = 0;
		uint32_t frame_width = 0;
		uint32_t frame_height = 0;
		uint32_t stride = 0;
		uint64_t timestamp = 0;
		if (!mpt_native_runtime_render_frame(runtime, &bgra, &size, &frame_width, &frame_height, &stride, &timestamp)) {
			std::cerr << "mpt_native_runtime_render_frame failed at frame " << frame_index << '\n';
			mpt_native_runtime_destroy(runtime);
			return 1;
		}

		if (!expect(bgra != nullptr, "native runtime should return a BGRA buffer") ||
		    !expect(frame_width == width && frame_height == height, "native runtime frame dimensions should stay stable") ||
		    !expect(stride == frame_width * 4U, "native runtime frame stride should be width * 4") ||
		    !expect(size == static_cast<size_t>(frame_height) * stride, "native runtime frame size should match dimensions")) {
			mpt_native_runtime_destroy(runtime);
			return 1;
		}

		if (frame_index > 0 && !expect(timestamp > previous_timestamp, "native runtime timestamps should be strictly monotonic")) {
			mpt_native_runtime_destroy(runtime);
			return 1;
		}

		unique_hashes.insert(fnv1a_64(bgra, size));
		previous_timestamp = timestamp;
	}

	mpt_native_runtime_destroy(runtime);

	if (!expect(unique_hashes.size() >= 3, "native runtime should render changing Linux frames with decoded PNG/NPZ assets"))
		return 1;

	return 0;
}
