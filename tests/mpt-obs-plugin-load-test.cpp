#include <obs-module.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

struct Options {
	std::filesystem::path plugin_path;
	std::filesystem::path plugin_data_dir;
	std::filesystem::path asset_dir;
};

std::optional<Options> parse_args(int argc, char **argv)
{
	Options options;
	for (int idx = 1; idx < argc; ++idx) {
		std::string arg = argv[idx];
		auto require_value = [&](const char *name) -> const char * {
			if (idx + 1 >= argc) {
				std::cerr << "missing value for " << name << '\n';
				return nullptr;
			}
			return argv[++idx];
		};

		if (arg == "--plugin-path") {
			const char *value = require_value("--plugin-path");
			if (!value)
				return std::nullopt;
			options.plugin_path = value;
		} else if (arg == "--plugin-data-dir") {
			const char *value = require_value("--plugin-data-dir");
			if (!value)
				return std::nullopt;
			options.plugin_data_dir = value;
		} else if (arg == "--asset-dir") {
			const char *value = require_value("--asset-dir");
			if (!value)
				return std::nullopt;
			options.asset_dir = value;
		} else {
			std::cerr << "unknown argument: " << arg << '\n';
			return std::nullopt;
		}
	}

	if (options.plugin_path.empty() || options.plugin_data_dir.empty() || options.asset_dir.empty()) {
		std::cerr << "--plugin-path, --plugin-data-dir, and --asset-dir are required\n";
		return std::nullopt;
	}
	return options;
}

bool has_registered_input(const char *wanted_id)
{
	for (size_t idx = 0;; ++idx) {
		const char *id = nullptr;
		if (!obs_enum_input_types(idx, &id))
			return false;
		if (id && std::string(id) == wanted_id)
			return true;
	}
}

} // namespace

int main(int argc, char **argv)
{
	auto parsed = parse_args(argc, argv);
	if (!parsed)
		return 2;
	const Options options = *parsed;

	if (!obs_startup("en-US", nullptr, nullptr)) {
		std::cerr << "obs_startup failed\n";
		return 1;
	}

	int exit_code = 1;
	obs_module_t *module = nullptr;
	obs_source_t *source = nullptr;
	obs_data_t *settings = nullptr;

	do {
		const int open_result =
			obs_open_module(&module, options.plugin_path.u8string().c_str(), options.plugin_data_dir.u8string().c_str());
		if (open_result != MODULE_SUCCESS || !module) {
			std::cerr << "obs_open_module failed with code " << open_result << '\n';
			break;
		}
		if (!obs_init_module(module)) {
			std::cerr << "obs_init_module failed\n";
			break;
		}
		obs_post_load_modules();

		if (!has_registered_input("motionpngtuber_player")) {
			std::cerr << "motionpngtuber_player input was not registered after loading the Ubuntu plugin\n";
			break;
		}

		settings = obs_data_create();
		if (!settings) {
			std::cerr << "obs_data_create failed\n";
			break;
		}

		obs_data_set_string(settings, "loop_video", (options.asset_dir / "loop_motion.mp4").u8string().c_str());
		obs_data_set_string(settings, "mouth_dir", (options.asset_dir / "mouth").u8string().c_str());
		obs_data_set_string(settings, "track_file", (options.asset_dir / "mouth_track.npz").u8string().c_str());
		obs_data_set_string(settings, "track_calibrated_file", "");
		obs_data_set_string(settings, "audio_sync_source_uuid", "");
		obs_data_set_string(settings, "audio_device_identity", "");
		obs_data_set_string(settings, "valid_policy", "hold");
		obs_data_set_int(settings, "audio_device_index", -1);
		obs_data_set_int(settings, "render_fps", 24);
		obs_data_set_bool(settings, "legacy_direct_audio_requested", false);

		source = obs_source_create("motionpngtuber_player", "Linux Validation Source", settings, nullptr);
		if (!source) {
			std::cerr << "obs_source_create failed for motionpngtuber_player\n";
			break;
		}

		bool ready = false;
		for (int attempt = 0; attempt < 50; ++attempt) {
			uint32_t width = obs_source_get_width(source);
			uint32_t height = obs_source_get_height(source);
			if (width == 64 && height == 64) {
				ready = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}

		if (!ready) {
			std::cerr << "loaded Ubuntu plugin did not produce a ready source within the timeout\n";
			break;
		}

		exit_code = 0;
	} while (false);

	if (source)
		obs_source_release(source);
	if (settings)
		obs_data_release(settings);
	obs_shutdown();
	return exit_code;
}
