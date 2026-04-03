#include "motionpngtuber-source.h"

#ifdef MPT_FALLBACK_OBS
#include "mpt-obs-module.h"
#include "mpt-obs-util.h"
#else
#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#endif

#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>

#include "motionpngtuber-native.h"
#include "mpt-text.h"
#include "plugin-support.h"

#define PROP_LOOP_VIDEO "loop_video"
#define PROP_MOUTH_DIR "mouth_dir"
#define PROP_TRACK_FILE "track_file"
#define PROP_TRACK_CALIBRATED_FILE "track_calibrated_file"
#define PROP_RENDER_FPS "render_fps"
#define PROP_STATUS_INFO "status_info"
#define PROP_AUDIO_SYNC_INFO "audio_sync_info"
#define PROP_AUDIO_SYNC_SOURCE_UUID "audio_sync_source_uuid"
#define PROP_AUDIO_DEVICE_IDENTITY "audio_device_identity"
#define PROP_AUDIO_DEVICE_INDEX "audio_device_index"
#define PROP_VALID_POLICY "valid_policy"
#define PROP_SHOW_ADVANCED "show_advanced"
#define AUTO_OBS_AUDIO_SOURCE_SELECTION "__auto__"
#define DIRECT_OBS_AUDIO_SOURCE_SELECTION "__direct__"
#define FRAME_BUFFER_RESIZE_ERROR "Failed to resize video frame buffer."

extern obs_data_t *obs_source_get_settings(const obs_source_t *source);

struct motionpngtuber_source {
	obs_source_t *source;
	os_event_t *stop_signal;
	pthread_t thread;
	pthread_mutex_t mutex;
	bool thread_started;
	bool active;
	bool runtime_ready;
	bool runtime_dirty;

	char *loop_video;
	char *mouth_dir;
	char *track_file;
	char *track_calibrated_file;
	char *audio_sync_source_uuid;
	char *resolved_audio_sync_source_uuid;
	char *audio_device_identity_json;
	char *valid_policy;
	char *last_error;

	long long render_fps;
	long long audio_device_index;

	uint8_t *frame_data;
	size_t frame_data_size;
	uint32_t frame_width;
	uint32_t frame_height;
	uint32_t frame_stride;

	struct mpt_native_runtime *runtime;
};

static void motionpngtuber_refresh_property_visibility(obs_properties_t *props, obs_data_t *settings);

static void replace_string(char **dst, const char *src)
{
	if (*dst) {
		bfree(*dst);
		*dst = NULL;
	}

	if (src && *src)
		*dst = bstrdup(src);
}

static bool has_text(const char *value)
{
	return value && *value;
}

static bool strings_equal_nullable(const char *lhs, const char *rhs)
{
	if (!lhs)
		lhs = "";
	if (!rhs)
		rhs = "";
	return strcmp(lhs, rhs) == 0;
}

static bool has_track_path(const struct motionpngtuber_source *context)
{
	return context && (has_text(context->track_file) || has_text(context->track_calibrated_file));
}

static bool path_has_separator(char ch)
{
	return ch == '\\' || ch == '/';
}

static char preferred_path_separator(void)
{
	return '\\';
}

static wchar_t *utf8_to_wide_dup(const char *text)
{
	if (!has_text(text))
		return NULL;

	int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	if (needed <= 0)
		return NULL;

	wchar_t *wide = bzalloc((size_t)needed * sizeof(wchar_t));
	if (!wide)
		return NULL;

	if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, needed) <= 0) {
		bfree(wide);
		return NULL;
	}
	return wide;
}

static bool path_is_directory(const char *path)
{
	if (!has_text(path))
		return false;
	wchar_t *path_w = utf8_to_wide_dup(path);
	if (!path_w)
		return false;
	DWORD attrs = GetFileAttributesW(path_w);
	bfree(path_w);
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool extract_parent_directory(const char *path, struct dstr *out)
{
	if (!has_text(path))
		return false;

	dstr_copy(out, path);
	if (!out->array || !out->len)
		return false;

	char *last_sep = NULL;
	for (char *cursor = out->array; *cursor; ++cursor) {
		if (path_has_separator(*cursor))
			last_sep = cursor;
	}
	if (!last_sep)
		return false;

	*last_sep = '\0';
	out->len = (size_t)(last_sep - out->array);
	return out->len > 0;
}

static void build_child_path(struct dstr *out, const char *dir, const char *name)
{
	dstr_copy(out, dir);
	if (out->len > 0 && !path_has_separator(out->array[out->len - 1]))
		dstr_cat_ch(out, preferred_path_separator());
	dstr_cat(out, name);
}

static void set_first_existing_file(obs_data_t *settings, const char *key, const char *dir, const char *const *names,
				    size_t name_count)
{
	struct dstr candidate = {0};
	for (size_t idx = 0; idx < name_count; ++idx) {
		build_child_path(&candidate, dir, names[idx]);
		if (os_file_exists(candidate.array)) {
			obs_data_set_string(settings, key, candidate.array);
			dstr_free(&candidate);
			return;
		}
	}

	obs_data_set_string(settings, key, "");
	dstr_free(&candidate);
}

static void auto_fill_related_paths(obs_data_t *settings)
{
	const char *loop_video = obs_data_get_string(settings, PROP_LOOP_VIDEO);
	struct dstr base_dir = {0};
	struct dstr mouth_dir = {0};
	const char *track_names[] = {"mouth_track.json", "mouth_track.npz"};
	const char *track_calibrated_names[] = {"mouth_track_calibrated.json", "mouth_track_calibrated.npz"};

	if (!extract_parent_directory(loop_video, &base_dir)) {
		obs_data_set_string(settings, PROP_MOUTH_DIR, "");
		obs_data_set_string(settings, PROP_TRACK_FILE, "");
		obs_data_set_string(settings, PROP_TRACK_CALIBRATED_FILE, "");
		dstr_free(&base_dir);
		return;
	}

	build_child_path(&mouth_dir, base_dir.array, "mouth");
	obs_data_set_string(settings, PROP_MOUTH_DIR, path_is_directory(mouth_dir.array) ? mouth_dir.array : "");
	set_first_existing_file(settings, PROP_TRACK_FILE, base_dir.array, track_names, 2);
	set_first_existing_file(settings, PROP_TRACK_CALIBRATED_FILE, base_dir.array, track_calibrated_names, 2);

	dstr_free(&mouth_dir);
	dstr_free(&base_dir);
}

static bool should_auto_fill_related_paths(obs_data_t *settings)
{
	return has_text(obs_data_get_string(settings, PROP_LOOP_VIDEO)) &&
	       !has_text(obs_data_get_string(settings, PROP_MOUTH_DIR)) &&
	       !has_text(obs_data_get_string(settings, PROP_TRACK_FILE)) &&
	       !has_text(obs_data_get_string(settings, PROP_TRACK_CALIBRATED_FILE));
}

static bool motionpngtuber_loop_video_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	auto_fill_related_paths(settings);
	motionpngtuber_refresh_property_visibility(props, settings);
	return true;
}

static bool motionpngtuber_audio_sync_source_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	motionpngtuber_refresh_property_visibility(props, settings);
	return true;
}

static bool motionpngtuber_show_advanced_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	motionpngtuber_refresh_property_visibility(props, settings);
	return true;
}

static void motionpngtuber_reset_runtime_locked(struct motionpngtuber_source *context)
{
	if (context->runtime) {
		mpt_native_runtime_destroy(context->runtime);
		context->runtime = NULL;
	}

	context->runtime_ready = false;
	context->frame_width = 0;
	context->frame_height = 0;
	context->frame_stride = 0;
}

static void apply_runtime_defaults(struct motionpngtuber_source *context)
{
	if (!has_text(context->valid_policy))
		replace_string(&context->valid_policy, "hold");
	if (context->render_fps <= 0)
		context->render_fps = 30;
}

static const char *normalize_audio_sync_source_uuid(const char *value)
{
	if (!has_text(value))
		return AUTO_OBS_AUDIO_SOURCE_SELECTION;
	if (strcmp(value, DIRECT_OBS_AUDIO_SOURCE_SELECTION) == 0)
		return "";
	return value;
}

static const char *canonicalize_audio_sync_source_uuid(const char *value)
{
	return has_text(value) ? value : DIRECT_OBS_AUDIO_SOURCE_SELECTION;
}

static bool audio_sync_source_uses_direct_device(const char *value)
{
	return !has_text(value) || strcmp(value, AUTO_OBS_AUDIO_SOURCE_SELECTION) == 0 ||
	       strcmp(value, DIRECT_OBS_AUDIO_SOURCE_SELECTION) == 0;
}

static bool advanced_settings_are_active(obs_data_t *settings)
{
	if (!settings)
		return false;
	if (has_text(obs_data_get_string(settings, PROP_TRACK_CALIBRATED_FILE)))
		return true;
	if (obs_data_get_int(settings, PROP_RENDER_FPS) > 0 && obs_data_get_int(settings, PROP_RENDER_FPS) != 30)
		return true;

	const char *valid_policy = obs_data_get_string(settings, PROP_VALID_POLICY);
	return has_text(valid_policy) && strcmp(valid_policy, "hold") != 0;
}

static void motionpngtuber_refresh_property_visibility(obs_properties_t *props, obs_data_t *settings)
{
	if (!props)
		return;

	const char *audio_sync_source_uuid = settings ? obs_data_get_string(settings, PROP_AUDIO_SYNC_SOURCE_UUID)
						      : AUTO_OBS_AUDIO_SOURCE_SELECTION;
	bool show_advanced = settings && obs_data_get_bool(settings, PROP_SHOW_ADVANCED);
	bool show_audio_device = audio_sync_source_uses_direct_device(audio_sync_source_uuid);

	obs_property_t *audio_device = obs_properties_get(props, PROP_AUDIO_DEVICE_IDENTITY);
	obs_property_t *track_calibrated = obs_properties_get(props, PROP_TRACK_CALIBRATED_FILE);
	obs_property_t *render_fps = obs_properties_get(props, PROP_RENDER_FPS);
	obs_property_t *valid_policy = obs_properties_get(props, PROP_VALID_POLICY);

	if (audio_device)
		obs_property_set_visible(audio_device, show_audio_device);
	if (track_calibrated)
		obs_property_set_visible(track_calibrated, show_advanced);
	if (render_fps)
		obs_property_set_visible(render_fps, show_advanced);
	if (valid_policy)
		obs_property_set_visible(valid_policy, show_advanced);
}

static bool contains_ascii_case_insensitive(const char *haystack, const char *needle)
{
	size_t needle_len = needle ? strlen(needle) : 0;
	if (!haystack || !needle || needle_len == 0)
		return false;

	for (const char *cursor = haystack; *cursor; ++cursor) {
		size_t idx = 0;
		while (needle[idx] && cursor[idx] &&
		       tolower((unsigned char)cursor[idx]) == tolower((unsigned char)needle[idx])) {
			++idx;
		}
		if (idx == needle_len)
			return true;
	}

	return false;
}

struct auto_obs_audio_source_selection {
	char *uuid;
	int score;
};

static int score_obs_audio_source_for_auto_follow(const obs_source_t *source)
{
	if (!source || (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
		return -1;

	int score = 100;
	const char *source_id = obs_source_get_unversioned_id(source);
	if (!has_text(source_id))
		source_id = obs_source_get_id(source);
	if (contains_ascii_case_insensitive(source_id, "wasapi_input_capture"))
		score += 250;
	else if (contains_ascii_case_insensitive(source_id, "wasapi_process_output_capture"))
		score += 125;
	else if (contains_ascii_case_insensitive(source_id, "wasapi_output_capture"))
		score += 100;
	else if (contains_ascii_case_insensitive(source_id, "input_capture"))
		score += 175;
	else if (contains_ascii_case_insensitive(source_id, "output_capture"))
		score += 75;

	const char *name = obs_source_get_name(source);
	if (contains_ascii_case_insensitive(name, "mic") ||
	    contains_ascii_case_insensitive(name, "microphone") ||
	    contains_ascii_case_insensitive(name, "input") ||
	    contains_ascii_case_insensitive(name, "line in"))
		score += 100;
	if (contains_ascii_case_insensitive(name, "desktop") ||
	    contains_ascii_case_insensitive(name, "speaker") ||
	    contains_ascii_case_insensitive(name, "output"))
		score += 50;
	if (contains_ascii_case_insensitive(name, "capture"))
		score += 25;
	return score;
}

static bool enum_obs_audio_source_for_auto_follow(void *param, obs_source_t *source)
{
	struct auto_obs_audio_source_selection *selection = param;
	const char *uuid = obs_source_get_uuid(source);
	int score = 0;

	if (!selection || !source || !has_text(uuid))
		return true;

	score = score_obs_audio_source_for_auto_follow(source);
	if (score < 0 || score <= selection->score)
		return true;

	replace_string(&selection->uuid, uuid);
	selection->score = score;
	return true;
}

static char *resolve_audio_sync_source_uuid_for_runtime(const char *selection)
{
	if (!has_text(selection))
		return NULL;
	if (strcmp(selection, AUTO_OBS_AUDIO_SOURCE_SELECTION) != 0)
		return bstrdup(selection);

	struct auto_obs_audio_source_selection auto_selection = {0};
	auto_selection.score = -1;
	obs_enum_sources(enum_obs_audio_source_for_auto_follow, &auto_selection);
	return auto_selection.uuid;
}

static void write_canonical_settings(obs_data_t *settings, const char *loop_video, const char *mouth_dir,
				       const char *track_file, const char *track_calibrated_file,
				       long long render_fps, const char *audio_sync_source_uuid, long long audio_device_index,
				       const char *audio_device_identity, const char *valid_policy)
{
	obs_data_set_string(settings, PROP_LOOP_VIDEO, loop_video ? loop_video : "");
	obs_data_set_string(settings, PROP_MOUTH_DIR, mouth_dir ? mouth_dir : "");
	obs_data_set_string(settings, PROP_TRACK_FILE, track_file ? track_file : "");
	obs_data_set_string(settings, PROP_TRACK_CALIBRATED_FILE,
			    track_calibrated_file ? track_calibrated_file : "");
	obs_data_set_int(settings, PROP_RENDER_FPS, render_fps > 0 ? render_fps : 30);
	obs_data_set_string(settings, PROP_AUDIO_SYNC_SOURCE_UUID,
			    canonicalize_audio_sync_source_uuid(audio_sync_source_uuid));
	obs_data_set_int(settings, PROP_AUDIO_DEVICE_INDEX, audio_device_index);
	obs_data_set_string(settings, PROP_AUDIO_DEVICE_IDENTITY, audio_device_identity ? audio_device_identity : "");
	obs_data_set_string(settings, PROP_VALID_POLICY, has_text(valid_policy) ? valid_policy : "hold");
}

static void motionpngtuber_rebuild_runtime_locked(struct motionpngtuber_source *context)
{
	if (!context->runtime_dirty)
		return;

	context->runtime_dirty = false;
	motionpngtuber_reset_runtime_locked(context);
	replace_string(&context->last_error, NULL);

	if (!has_text(context->loop_video) || !has_text(context->mouth_dir) || !has_track_path(context))
		return;

	struct mpt_native_runtime_config config = {
		.loop_video = context->loop_video,
		.mouth_dir = context->mouth_dir,
		.track_file = context->track_file,
		.track_calibrated_file = context->track_calibrated_file,
		.audio_sync_source_uuid = context->resolved_audio_sync_source_uuid,
		.audio_device_identity_json = context->audio_device_identity_json,
		.valid_policy = context->valid_policy,
		.audio_device_index = context->audio_device_index,
		.render_fps = context->render_fps,
	};

	char *error_text = NULL;
	if (!mpt_native_runtime_create(&context->runtime, &config, &error_text)) {
		replace_string(&context->last_error, error_text ? error_text : "Native runtime initialization failed.");
		obs_log(LOG_ERROR, "failed to initialize MotionPngTuberPlayer runtime: %s",
			context->last_error ? context->last_error : "Native runtime initialization failed.");
		if (error_text)
			bfree(error_text);
		return;
	}

	if (error_text)
		bfree(error_text);

	context->runtime_ready = true;
	mpt_native_runtime_get_dimensions(context->runtime, &context->frame_width, &context->frame_height);
	context->frame_stride = context->frame_width * 4U;
}

static char *build_status_text(struct motionpngtuber_source *context)
{
	struct dstr text = {0};

	if (!context || !has_text(context->loop_video) || !has_text(context->mouth_dir) || !has_track_path(context)) {
		dstr_copy(&text, mpt_text("MotionPngTuberPlayer.StatusConfigureSource"));
		return text.array;
	}

	if (has_text(context->last_error)) {
		dstr_copy(&text, context->last_error);
		return text.array;
	}

	if (!context->runtime_ready) {
		dstr_copy(&text, mpt_text("MotionPngTuberPlayer.StatusWorkerStarting"));
		return text.array;
	}

	if (context->frame_width > 0 && context->frame_height > 0) {
		dstr_catf(&text, "%s: %ux%u @ %lld FPS", mpt_text("MotionPngTuberPlayer.StatusRunning"),
			  context->frame_width, context->frame_height, context->render_fps);
		return text.array;
	}

	dstr_copy(&text, mpt_text("MotionPngTuberPlayer.StatusWaitingFrame"));
	return text.array;
}

static void *motionpngtuber_video_thread(void *data)
{
	struct motionpngtuber_source *context = data;
	uint64_t next_frame_time = os_gettime_ns();

	while (os_event_try(context->stop_signal) == EAGAIN) {
		struct obs_source_frame frame = {0};
		uint64_t frame_interval_ns = 1000000000ULL / 30ULL;
		bool has_frame = false;

		pthread_mutex_lock(&context->mutex);
		if (context->render_fps > 0)
			frame_interval_ns = 1000000000ULL / (uint64_t)context->render_fps;

		motionpngtuber_rebuild_runtime_locked(context);
		bool should_render = context->active && context->runtime != NULL;
		if (should_render) {
			uint8_t *native_frame = NULL;
			size_t native_size = 0;
			uint32_t native_width = 0;
			uint32_t native_height = 0;
			uint32_t native_stride = 0;
			uint64_t native_timestamp = 0;
			if (mpt_native_runtime_render_frame(context->runtime, &native_frame, &native_size, &native_width, &native_height,
						     &native_stride, &native_timestamp) &&
			    native_frame && native_size > 0) {
				if (native_size > context->frame_data_size) {
					uint8_t *resized = brealloc(context->frame_data, native_size);
					if (resized) {
						context->frame_data = resized;
						context->frame_data_size = native_size;
						if (strings_equal_nullable(context->last_error, FRAME_BUFFER_RESIZE_ERROR))
							replace_string(&context->last_error, NULL);
					} else if (!strings_equal_nullable(context->last_error, FRAME_BUFFER_RESIZE_ERROR)) {
						replace_string(&context->last_error, FRAME_BUFFER_RESIZE_ERROR);
						obs_log(LOG_ERROR, "failed to resize MotionPngTuberPlayer frame buffer to %zu bytes", native_size);
					}
				}

				if (context->frame_data && native_size <= context->frame_data_size) {
					memcpy(context->frame_data, native_frame, native_size);
					context->frame_width = native_width;
					context->frame_height = native_height;
					context->frame_stride = native_stride;
					frame.data[0] = context->frame_data;
					frame.linesize[0] = native_stride;
					frame.width = native_width;
					frame.height = native_height;
					frame.format = VIDEO_FORMAT_BGRA;
					frame.timestamp = native_timestamp;
					has_frame = true;
				}
			}
		}
		pthread_mutex_unlock(&context->mutex);

		if (has_frame)
			obs_source_output_video(context->source, &frame);

		os_sleepto_ns(next_frame_time += frame_interval_ns);
	}

	pthread_mutex_lock(&context->mutex);
	motionpngtuber_reset_runtime_locked(context);
	pthread_mutex_unlock(&context->mutex);
	return NULL;
}

static void motionpngtuber_update(void *data, obs_data_t *settings)
{
	struct motionpngtuber_source *context = data;
	obs_data_t *source_settings = NULL;
	obs_data_t *effective_settings = settings;
	obs_data_t *merged_settings = NULL;
	obs_data_t *auto_fill_probe = NULL;
	const char *loop_video = NULL;
	const char *mouth_dir = NULL;
	const char *track_file = NULL;
	const char *track_calibrated_file = NULL;
	const char *audio_sync_source_uuid = NULL;
	const char *audio_device_identity = NULL;
	const char *valid_policy = NULL;
	const char *auto_filled_track_file = NULL;
	char *saved_loop_video = NULL;
	char *saved_mouth_dir = NULL;
	char *saved_track_file = NULL;
	char *saved_track_calibrated_file = NULL;
	char *saved_audio_sync_source_uuid = NULL;
	char *resolved_audio_sync_source_uuid = NULL;
	char *saved_audio_device_identity = NULL;
	char *saved_valid_policy = NULL;
	long long render_fps = 0;
	long long normalized_render_fps = 30;
	long long saved_render_fps = 30;
	long long audio_device_index = -1;
	long long saved_audio_device_index = -1;
	bool preserve_current_track_file = false;
	bool requires_runtime_rebuild = false;

	if (context->source) {
		/* Video source updates can arrive as partial overlays, so read the merged source settings. */
		source_settings = obs_source_get_settings(context->source);
		if (source_settings)
			effective_settings = source_settings;
	}

	merged_settings = obs_data_create();
	if (effective_settings) {
		obs_data_apply(merged_settings, effective_settings);
	} else {
		pthread_mutex_lock(&context->mutex);
		write_canonical_settings(merged_settings, context->loop_video, context->mouth_dir, context->track_file,
					 context->track_calibrated_file, context->render_fps, context->audio_sync_source_uuid,
					 context->audio_device_index, context->audio_device_identity_json, context->valid_policy);
		pthread_mutex_unlock(&context->mutex);
		if (settings)
			obs_data_apply(merged_settings, settings);
	}

	if (should_auto_fill_related_paths(merged_settings))
		auto_fill_related_paths(merged_settings);

	loop_video = obs_data_get_string(merged_settings, PROP_LOOP_VIDEO);
	mouth_dir = obs_data_get_string(merged_settings, PROP_MOUTH_DIR);
	track_file = obs_data_get_string(merged_settings, PROP_TRACK_FILE);
	track_calibrated_file = obs_data_get_string(merged_settings, PROP_TRACK_CALIBRATED_FILE);
	audio_sync_source_uuid =
		normalize_audio_sync_source_uuid(obs_data_get_string(merged_settings, PROP_AUDIO_SYNC_SOURCE_UUID));
	resolved_audio_sync_source_uuid = resolve_audio_sync_source_uuid_for_runtime(audio_sync_source_uuid);
	audio_device_identity = obs_data_get_string(merged_settings, PROP_AUDIO_DEVICE_IDENTITY);
	valid_policy = obs_data_get_string(merged_settings, PROP_VALID_POLICY);
	if (!has_text(valid_policy))
		valid_policy = "hold";
	render_fps = obs_data_get_int(merged_settings, PROP_RENDER_FPS);
	if (render_fps > 0)
		normalized_render_fps = render_fps;
	audio_device_index = obs_data_get_int(merged_settings, PROP_AUDIO_DEVICE_INDEX);
	if (has_text(loop_video)) {
		auto_fill_probe = obs_data_create();
		if (auto_fill_probe) {
			obs_data_set_string(auto_fill_probe, PROP_LOOP_VIDEO, loop_video);
			auto_fill_related_paths(auto_fill_probe);
			auto_filled_track_file = obs_data_get_string(auto_fill_probe, PROP_TRACK_FILE);
		}
	}

	pthread_mutex_lock(&context->mutex);
	preserve_current_track_file =
		has_text(context->loop_video) && has_text(loop_video) &&
		strings_equal_nullable(context->loop_video, loop_video) &&
		has_text(context->track_file) && has_text(track_file) &&
		!strings_equal_nullable(context->track_file, track_file) &&
		has_text(auto_filled_track_file) &&
		strings_equal_nullable(track_file, auto_filled_track_file) &&
		!strings_equal_nullable(context->track_file, auto_filled_track_file) &&
		(context->render_fps != normalized_render_fps ||
		 !strings_equal_nullable(context->valid_policy, valid_policy) ||
		 !strings_equal_nullable(context->audio_sync_source_uuid, audio_sync_source_uuid) ||
		 !strings_equal_nullable(context->audio_device_identity_json, audio_device_identity) ||
		 context->audio_device_index != audio_device_index);
	if (preserve_current_track_file)
		track_file = context->track_file;
	requires_runtime_rebuild = !strings_equal_nullable(context->loop_video, loop_video) ||
				   !strings_equal_nullable(context->mouth_dir, mouth_dir) ||
				   !strings_equal_nullable(context->track_file, track_file) ||
				   !strings_equal_nullable(context->track_calibrated_file, track_calibrated_file) ||
				   !strings_equal_nullable(context->audio_sync_source_uuid, audio_sync_source_uuid) ||
				   !strings_equal_nullable(context->resolved_audio_sync_source_uuid,
							   resolved_audio_sync_source_uuid) ||
				   !strings_equal_nullable(context->audio_device_identity_json, audio_device_identity) ||
				   !strings_equal_nullable(context->valid_policy, valid_policy) ||
				   context->render_fps != normalized_render_fps ||
				   context->audio_device_index != audio_device_index;

	replace_string(&context->loop_video, loop_video);
	replace_string(&context->mouth_dir, mouth_dir);
	replace_string(&context->track_file, track_file);
	replace_string(&context->track_calibrated_file, track_calibrated_file);
	replace_string(&context->audio_sync_source_uuid, audio_sync_source_uuid);
	replace_string(&context->resolved_audio_sync_source_uuid, resolved_audio_sync_source_uuid);
	replace_string(&context->audio_device_identity_json, audio_device_identity);
	replace_string(&context->valid_policy, valid_policy);

	context->render_fps = normalized_render_fps;
	context->audio_device_index = audio_device_index;
	apply_runtime_defaults(context);
	saved_loop_video = bstrdup(context->loop_video ? context->loop_video : "");
	saved_mouth_dir = bstrdup(context->mouth_dir ? context->mouth_dir : "");
	saved_track_file = bstrdup(context->track_file ? context->track_file : "");
	saved_track_calibrated_file = bstrdup(context->track_calibrated_file ? context->track_calibrated_file : "");
	saved_audio_sync_source_uuid = bstrdup(context->audio_sync_source_uuid ? context->audio_sync_source_uuid : "");
	saved_audio_device_identity = bstrdup(context->audio_device_identity_json ? context->audio_device_identity_json : "");
	saved_valid_policy = bstrdup((context->valid_policy && *context->valid_policy) ? context->valid_policy : "hold");
	saved_render_fps = context->render_fps > 0 ? context->render_fps : 30;
	saved_audio_device_index = context->audio_device_index;
	if (requires_runtime_rebuild)
		context->runtime_dirty = true;
	pthread_mutex_unlock(&context->mutex);

	write_canonical_settings(effective_settings, saved_loop_video, saved_mouth_dir, saved_track_file,
				 saved_track_calibrated_file, saved_render_fps, saved_audio_sync_source_uuid,
				 saved_audio_device_index,
				 saved_audio_device_identity, saved_valid_policy);
	if (effective_settings != settings) {
		write_canonical_settings(settings, saved_loop_video, saved_mouth_dir, saved_track_file,
					 saved_track_calibrated_file, saved_render_fps,
					 saved_audio_sync_source_uuid, saved_audio_device_index,
					 saved_audio_device_identity, saved_valid_policy);
	}

	bfree(saved_loop_video);
	bfree(saved_mouth_dir);
	bfree(saved_track_file);
	bfree(saved_track_calibrated_file);
	bfree(saved_audio_sync_source_uuid);
	bfree(resolved_audio_sync_source_uuid);
	bfree(saved_audio_device_identity);
	bfree(saved_valid_policy);
	if (auto_fill_probe)
		obs_data_release(auto_fill_probe);
	if (merged_settings)
		obs_data_release(merged_settings);
	if (source_settings)
		obs_data_release(source_settings);
}

static void motionpngtuber_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, PROP_RENDER_FPS, 30);
	obs_data_set_default_string(settings, PROP_AUDIO_SYNC_SOURCE_UUID, AUTO_OBS_AUDIO_SOURCE_SELECTION);
	obs_data_set_default_int(settings, PROP_AUDIO_DEVICE_INDEX, -1);
	obs_data_set_default_string(settings, PROP_AUDIO_DEVICE_IDENTITY, "");
	obs_data_set_default_string(settings, PROP_VALID_POLICY, "hold");
	obs_data_set_default_bool(settings, PROP_SHOW_ADVANCED, false);
}

static obs_properties_t *motionpngtuber_properties(void *data)
{
	struct motionpngtuber_source *context = data;
	obs_properties_t *props = obs_properties_create();
	obs_data_t *property_settings = NULL;
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	char *status_text = NULL;
	if (context) {
		pthread_mutex_lock(&context->mutex);
		status_text = build_status_text(context);
		pthread_mutex_unlock(&context->mutex);
	} else {
		status_text = build_status_text(NULL);
	}
	obs_properties_add_text(props, PROP_STATUS_INFO, status_text, OBS_TEXT_INFO);
	bfree(status_text);

	obs_property_t *loop_video = obs_properties_add_path(props, PROP_LOOP_VIDEO,
						 mpt_text("MotionPngTuberPlayer.LoopVideo"), OBS_PATH_FILE,
						 "MP4 Files (*.mp4);;All Files (*.*)", NULL);
	obs_property_set_modified_callback(loop_video, motionpngtuber_loop_video_modified);
	obs_properties_add_path(props, PROP_MOUTH_DIR, mpt_text("MotionPngTuberPlayer.MouthDir"), OBS_PATH_DIRECTORY,
				NULL, NULL);
	obs_properties_add_path(props, PROP_TRACK_FILE, mpt_text("MotionPngTuberPlayer.TrackFile"), OBS_PATH_FILE,
				"Track Files (*.json *.npz);;JSON Files (*.json);;NPZ Files (*.npz);;All Files (*.*)", NULL);
	obs_property_t *show_advanced =
		obs_properties_add_bool(props, PROP_SHOW_ADVANCED, mpt_text("MotionPngTuberPlayer.ShowAdvanced"));
	obs_property_set_modified_callback(show_advanced, motionpngtuber_show_advanced_modified);
	obs_properties_add_path(props, PROP_TRACK_CALIBRATED_FILE,
				mpt_text("MotionPngTuberPlayer.TrackCalibratedFile"), OBS_PATH_FILE,
				"Track Files (*.json *.npz);;JSON Files (*.json);;NPZ Files (*.npz);;All Files (*.*)", NULL);

	obs_properties_add_int(props, PROP_RENDER_FPS, mpt_text("MotionPngTuberPlayer.RenderFps"), 1, 120, 1);
	obs_properties_add_text(props, PROP_AUDIO_SYNC_INFO, mpt_text("MotionPngTuberPlayer.AudioSyncSourceInfo"),
				OBS_TEXT_INFO);
	obs_property_t *audio_sync_source =
		obs_properties_add_list(props, PROP_AUDIO_SYNC_SOURCE_UUID,
					 mpt_text("MotionPngTuberPlayer.AudioSyncSource"),
					 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	mpt_native_populate_obs_audio_sources(audio_sync_source);
	obs_property_set_modified_callback(audio_sync_source, motionpngtuber_audio_sync_source_modified);
	obs_property_t *audio_device = obs_properties_add_list(props, PROP_AUDIO_DEVICE_IDENTITY,
						    mpt_text("MotionPngTuberPlayer.AudioDevice"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	mpt_native_populate_audio_devices(audio_device);

	obs_property_t *policy = obs_properties_add_list(props, PROP_VALID_POLICY,
						 mpt_text("MotionPngTuberPlayer.ValidPolicy"),
						 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(policy, mpt_text("MotionPngTuberPlayer.Hold"), "hold");
	obs_property_list_add_string(policy, mpt_text("MotionPngTuberPlayer.Strict"), "strict");
	if (context && context->source) {
		property_settings = obs_source_get_settings(context->source);
		if (property_settings && advanced_settings_are_active(property_settings))
			obs_data_set_bool(property_settings, PROP_SHOW_ADVANCED, true);
	}
	motionpngtuber_refresh_property_visibility(props, property_settings);
	if (property_settings)
		obs_data_release(property_settings);
	return props;
}

static void motionpngtuber_show(void *data)
{
	struct motionpngtuber_source *context = data;
	pthread_mutex_lock(&context->mutex);
	context->active = true;
	pthread_mutex_unlock(&context->mutex);
}

static void motionpngtuber_hide(void *data)
{
	struct motionpngtuber_source *context = data;
	pthread_mutex_lock(&context->mutex);
	context->active = false;
	pthread_mutex_unlock(&context->mutex);
}

static uint32_t motionpngtuber_get_width(void *data)
{
	struct motionpngtuber_source *context = data;
	uint32_t width = 0;
	pthread_mutex_lock(&context->mutex);
	width = context->frame_width;
	pthread_mutex_unlock(&context->mutex);
	return width;
}

static uint32_t motionpngtuber_get_height(void *data)
{
	struct motionpngtuber_source *context = data;
	uint32_t height = 0;
	pthread_mutex_lock(&context->mutex);
	height = context->frame_height;
	pthread_mutex_unlock(&context->mutex);
	return height;
}

static void motionpngtuber_destroy(void *data)
{
	struct motionpngtuber_source *context = data;
	if (!context)
		return;

	os_event_signal(context->stop_signal);
	if (context->thread_started)
		pthread_join(context->thread, NULL);

	if (context->frame_data)
		bfree(context->frame_data);

	replace_string(&context->loop_video, NULL);
	replace_string(&context->mouth_dir, NULL);
	replace_string(&context->track_file, NULL);
	replace_string(&context->track_calibrated_file, NULL);
	replace_string(&context->audio_sync_source_uuid, NULL);
	replace_string(&context->resolved_audio_sync_source_uuid, NULL);
	replace_string(&context->audio_device_identity_json, NULL);
	replace_string(&context->valid_policy, NULL);
	replace_string(&context->last_error, NULL);

	pthread_mutex_destroy(&context->mutex);
	os_event_destroy(context->stop_signal);
	bfree(context);
}

static void *motionpngtuber_create(obs_data_t *settings, obs_source_t *source)
{
	struct motionpngtuber_source *context = bzalloc(sizeof(struct motionpngtuber_source));
	context->source = source;
	context->active = true;
	context->runtime_dirty = true;

	if (pthread_mutex_init(&context->mutex, NULL) != 0) {
		bfree(context);
		return NULL;
	}

	if (os_event_init(&context->stop_signal, OS_EVENT_TYPE_MANUAL) != 0) {
		pthread_mutex_destroy(&context->mutex);
		bfree(context);
		return NULL;
	}

	motionpngtuber_update(context, settings);

	if (pthread_create(&context->thread, NULL, motionpngtuber_video_thread, context) != 0) {
		motionpngtuber_destroy(context);
		return NULL;
	}

	context->thread_started = true;
	return context;
}

static const char *motionpngtuber_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return mpt_text("MotionPngTuberPlayer.SourceName");
}

struct obs_source_info motionpngtuber_source_info = {
	.id = "motionpngtuber_player",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = motionpngtuber_get_name,
	.create = motionpngtuber_create,
	.destroy = motionpngtuber_destroy,
	.update = motionpngtuber_update,
	.get_defaults = motionpngtuber_defaults,
	.get_properties = motionpngtuber_properties,
	.show = motionpngtuber_show,
	.hide = motionpngtuber_hide,
	.get_width = motionpngtuber_get_width,
	.get_height = motionpngtuber_get_height,
};
