#pragma once

#include "mpt-image-backend.h"

#include <cstdint>
#include <string>

struct MptVideoBackend;

bool mpt_video_backend_create(MptVideoBackend **out_backend, std::string &error);
void mpt_video_backend_destroy(MptVideoBackend *backend);
bool mpt_video_backend_open_loop_video(MptVideoBackend *backend, const std::string &loop_video_path, ImageBGRA *out_frame,
				       std::string &error);
bool mpt_video_backend_read_next_frame(MptVideoBackend *backend, ImageBGRA &image, uint64_t &timestamp_ns);
