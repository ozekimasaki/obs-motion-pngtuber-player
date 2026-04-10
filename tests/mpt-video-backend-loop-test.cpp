#include "mpt-video-backend.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace {

struct Options {
	std::string ffmpeg = "ffmpeg";
	std::filesystem::path work_dir;
	int frames = 20000;
	int max_same_hash_run = 4;
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

		if (arg == "--ffmpeg") {
			const char *value = require_value("--ffmpeg");
			if (!value)
				return std::nullopt;
			options.ffmpeg = value;
		} else if (arg == "--work-dir") {
			const char *value = require_value("--work-dir");
			if (!value)
				return std::nullopt;
			options.work_dir = value;
		} else if (arg == "--frames") {
			const char *value = require_value("--frames");
			if (!value)
				return std::nullopt;
			options.frames = std::max(1, std::stoi(value));
		} else if (arg == "--max-same-hash-run") {
			const char *value = require_value("--max-same-hash-run");
			if (!value)
				return std::nullopt;
			options.max_same_hash_run = std::max(1, std::stoi(value));
		} else {
			std::cerr << "unknown argument: " << arg << '\n';
			return std::nullopt;
		}
	}

	if (options.work_dir.empty()) {
		std::cerr << "--work-dir is required\n";
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

#if defined(_WIN32)

std::wstring utf8_to_wide(const std::string &value)
{
	if (value.empty())
		return std::wstring();
	int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
	if (needed <= 0)
		return std::wstring();
	std::wstring wide((size_t)needed, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), needed);
	if (!wide.empty() && wide.back() == L'\0')
		wide.pop_back();
	return wide;
}

std::wstring quote_windows_arg(const std::wstring &value)
{
	std::wstring escaped = L"\"";
	for (wchar_t ch : value) {
		if (ch == L'"' || ch == L'\\')
			escaped.push_back(L'\\');
		escaped.push_back(ch);
	}
	escaped.push_back(L'"');
	return escaped;
}

bool run_process(const Options &options, const std::filesystem::path &video_path)
{
	std::wostringstream command;
	command << quote_windows_arg(utf8_to_wide(options.ffmpeg))
		<< L" -y -hide_banner -loglevel error"
		<< L" -f lavfi -i "
		<< quote_windows_arg(L"testsrc2=size=320x240:rate=24:duration=2")
		<< L" -pix_fmt yuv420p "
		<< quote_windows_arg(video_path.wstring());

	STARTUPINFOW startup_info = {};
	startup_info.cb = sizeof(startup_info);
	PROCESS_INFORMATION process_info = {};
	std::wstring mutable_command_line = command.str();
	if (!CreateProcessW(nullptr, mutable_command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup_info,
			    &process_info)) {
		return false;
	}

	WaitForSingleObject(process_info.hProcess, INFINITE);
	DWORD exit_code = 1;
	GetExitCodeProcess(process_info.hProcess, &exit_code);
	CloseHandle(process_info.hThread);
	CloseHandle(process_info.hProcess);
	return exit_code == 0;
}

#else

bool run_process(const Options &options, const std::filesystem::path &video_path)
{
	pid_t child = -1;
	std::string filter = "testsrc2=size=320x240:rate=24:duration=2";
	std::string output_path = video_path.string();
	std::vector<char *> argv = {
		const_cast<char *>(options.ffmpeg.c_str()),
		const_cast<char *>("-y"),
		const_cast<char *>("-hide_banner"),
		const_cast<char *>("-loglevel"),
		const_cast<char *>("error"),
		const_cast<char *>("-f"),
		const_cast<char *>("lavfi"),
		const_cast<char *>("-i"),
		filter.data(),
		const_cast<char *>("-pix_fmt"),
		const_cast<char *>("yuv420p"),
		output_path.data(),
		nullptr,
	};

	if (posix_spawnp(&child, options.ffmpeg.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
		return false;

	int status = 0;
	if (waitpid(child, &status, 0) < 0)
		return false;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#endif

bool generate_motion_video(const Options &options, const std::filesystem::path &video_path)
{
	std::filesystem::create_directories(options.work_dir);
	if (!run_process(options, video_path)) {
		std::cerr << "ffmpeg failed while generating loop fixture\n";
		return false;
	}
	return std::filesystem::is_regular_file(video_path);
}

} // namespace

int main(int argc, char **argv)
{
	auto parsed = parse_args(argc, argv);
	if (!parsed)
		return 2;
	const Options options = *parsed;

	const std::filesystem::path video_path = options.work_dir / "loop_motion.mp4";
	if (!generate_motion_video(options, video_path))
		return 1;

	std::string error;
	MptVideoBackend *backend = nullptr;
	if (!mpt_video_backend_create(&backend, error) || !backend) {
		std::cerr << "mpt_video_backend_create failed: " << error << '\n';
		return 1;
	}

	ImageBGRA frame;
	if (!mpt_video_backend_open_loop_video(backend, video_path.u8string(), &frame, error)) {
		std::cerr << "mpt_video_backend_open_loop_video failed: " << error << '\n';
		mpt_video_backend_destroy(backend);
		return 1;
	}

	error.clear();
	const std::filesystem::path missing_video_path = options.work_dir / "missing-loop.mp4";
	if (mpt_video_backend_open_loop_video(backend, missing_video_path.u8string(), &frame, error)) {
		std::cerr << "mpt_video_backend_open_loop_video unexpectedly succeeded for a missing video\n";
		mpt_video_backend_destroy(backend);
		return 1;
	}
	if (error.empty()) {
		std::cerr << "mpt_video_backend_open_loop_video returned no error for a missing video\n";
		mpt_video_backend_destroy(backend);
		return 1;
	}

	error.clear();
	if (!mpt_video_backend_open_loop_video(backend, video_path.u8string(), &frame, error)) {
		std::cerr << "mpt_video_backend_open_loop_video failed after reset path: " << error << '\n';
		mpt_video_backend_destroy(backend);
		return 1;
	}

	uint64_t previous_hash = 0;
	uint64_t previous_timestamp = 0;
	int same_hash_run = 0;
	int hash_changes = 0;
	int loop_resets = 0;

	for (int frame_index = 0; frame_index < options.frames; ++frame_index) {
		uint64_t timestamp_ns = 0;
		if (!mpt_video_backend_read_next_frame(backend, frame, timestamp_ns)) {
			std::cerr << "mpt_video_backend_read_next_frame failed at frame " << frame_index << '\n';
			mpt_video_backend_destroy(backend);
			return 1;
		}

		if (frame.empty()) {
			std::cerr << "empty frame returned at frame " << frame_index << '\n';
			mpt_video_backend_destroy(backend);
			return 1;
		}

		uint64_t current_hash = fnv1a_64(frame.pixels.data(), frame.pixels.size());
		if (frame_index == 0) {
			same_hash_run = 1;
		} else {
			if (current_hash == previous_hash) {
				++same_hash_run;
			} else {
				same_hash_run = 1;
				++hash_changes;
			}

			if (timestamp_ns < previous_timestamp)
				++loop_resets;
		}

		if (same_hash_run > options.max_same_hash_run) {
			std::cerr << "detected stalled loop playback after frame " << frame_index
				  << " (same hash run " << same_hash_run << ")\n";
			mpt_video_backend_destroy(backend);
			return 1;
		}

		previous_hash = current_hash;
		previous_timestamp = timestamp_ns;
	}

	mpt_video_backend_destroy(backend);

	if (hash_changes < 100) {
		std::cerr << "video backend produced too few changing frames (" << hash_changes << ")\n";
		return 1;
	}
	if (loop_resets < 10) {
		std::cerr << "video backend did not loop enough times (" << loop_resets << ")\n";
		return 1;
	}

	return 0;
}
