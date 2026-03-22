#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(param) (void)(param)
#endif

#ifdef MPT_FALLBACK_OBS
#include "mpt-obs-module.h"
#include "mpt-obs-util.h"
#else
#include <obs-module.h>
#include <util/bmem.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct mpt_native_runtime;

struct mpt_native_runtime_config {
	const char *loop_video;
	const char *mouth_dir;
	const char *track_file;
	const char *track_calibrated_file;
	const char *audio_device_identity_json;
	const char *valid_policy;
	long long audio_device_index;
	long long render_fps;
};

void mpt_native_populate_audio_devices(obs_property_t *list);

bool mpt_native_runtime_create(struct mpt_native_runtime **out_runtime,
			       const struct mpt_native_runtime_config *config, char **error_text);
void mpt_native_runtime_destroy(struct mpt_native_runtime *runtime);

bool mpt_native_runtime_render_frame(struct mpt_native_runtime *runtime, uint8_t **out_bgra, size_t *out_size,
				     uint32_t *out_width, uint32_t *out_height, uint32_t *out_stride,
				     uint64_t *out_timestamp);

void mpt_native_runtime_get_dimensions(struct mpt_native_runtime *runtime, uint32_t *out_width, uint32_t *out_height);

#ifdef __cplusplus
}
#endif
