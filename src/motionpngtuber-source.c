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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "motionpngtuber-native.h"
#include "mpt-text.h"
#include "plugin-support.h"

#define PROP_LOOP_VIDEO "loop_video"
#define PROP_MOUTH_DIR "mouth_dir"
#define PROP_TRACK_FILE "track_file"
#define PROP_TRACK_CALIBRATED_FILE "track_calibrated_file"
#define PROP_RENDER_FPS "render_fps"
#define PROP_STATUS_INFO "status_info"
#define PROP_AUDIO_DEVICE_IDENTITY "audio_device_identity"
#define PROP_AUDIO_DEVICE_INDEX "audio_device_index"
#define PROP_VALID_POLICY "valid_policy"
#define FRAME_BUFFER_RESIZE_ERROR "Failed to resize video frame buffer."

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
#ifdef _WIN32
	return '\\';
#else
	return '/';
#endif
}

#ifdef _WIN32
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
#endif

static bool path_is_directory(const char *path)
{
	if (!has_text(path))
		return false;
#ifdef _WIN32
	wchar_t *path_w = utf8_to_wide_dup(path);
	if (!path_w)
		return false;
	DWORD attrs = GetFileAttributesW(path_w);
	bfree(path_w);
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
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
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	auto_fill_related_paths(settings);
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
		.audio_device_identity_json = context->audio_device_identity_json,
		.valid_policy = context->valid_policy,
		.audio_device_index = context->audio_device_index,
		.render_fps = context->render_fps,
	};

	char *error_text = NULL;
	if (!mpt_native_runtime_create(&context->runtime, &config, &error_text)) {
		replace_string(&context->last_error, error_text ? error_text : "Native runtime initialization failed.");
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
		if (context->active && context->runtime) {
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
	const char *loop_video = NULL;
	const char *mouth_dir = NULL;
	const char *track_file = NULL;
	const char *track_calibrated_file = NULL;
	const char *audio_device_identity = NULL;
	const char *valid_policy = NULL;
	long long render_fps = 0;
	long long audio_device_index = -1;
	bool requires_runtime_rebuild = false;

	if (should_auto_fill_related_paths(settings))
		auto_fill_related_paths(settings);

	loop_video = obs_data_get_string(settings, PROP_LOOP_VIDEO);
	mouth_dir = obs_data_get_string(settings, PROP_MOUTH_DIR);
	track_file = obs_data_get_string(settings, PROP_TRACK_FILE);
	track_calibrated_file = obs_data_get_string(settings, PROP_TRACK_CALIBRATED_FILE);
	audio_device_identity = obs_data_get_string(settings, PROP_AUDIO_DEVICE_IDENTITY);
	valid_policy = obs_data_get_string(settings, PROP_VALID_POLICY);
	render_fps = obs_data_get_int(settings, PROP_RENDER_FPS);
	audio_device_index = obs_data_get_int(settings, PROP_AUDIO_DEVICE_INDEX);

	pthread_mutex_lock(&context->mutex);
	requires_runtime_rebuild = !strings_equal_nullable(context->loop_video, loop_video) ||
				   !strings_equal_nullable(context->mouth_dir, mouth_dir) ||
				   !strings_equal_nullable(context->track_file, track_file) ||
				   !strings_equal_nullable(context->track_calibrated_file, track_calibrated_file) ||
				   !strings_equal_nullable(context->audio_device_identity_json, audio_device_identity) ||
				   !strings_equal_nullable(context->valid_policy, valid_policy) ||
				   context->audio_device_index != audio_device_index;

	replace_string(&context->loop_video, loop_video);
	replace_string(&context->mouth_dir, mouth_dir);
	replace_string(&context->track_file, track_file);
	replace_string(&context->track_calibrated_file, track_calibrated_file);
	replace_string(&context->audio_device_identity_json, audio_device_identity);
	replace_string(&context->valid_policy, valid_policy);

	context->render_fps = render_fps;
	context->audio_device_index = audio_device_index;
	apply_runtime_defaults(context);
	if (requires_runtime_rebuild)
		context->runtime_dirty = true;
	pthread_mutex_unlock(&context->mutex);
}

static void motionpngtuber_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, PROP_RENDER_FPS, 30);
	obs_data_set_default_int(settings, PROP_AUDIO_DEVICE_INDEX, -1);
	obs_data_set_default_string(settings, PROP_AUDIO_DEVICE_IDENTITY, "");
	obs_data_set_default_string(settings, PROP_VALID_POLICY, "hold");
}

static obs_properties_t *motionpngtuber_properties(void *data)
{
	struct motionpngtuber_source *context = data;
	obs_properties_t *props = obs_properties_create();
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
	obs_properties_add_path(props, PROP_TRACK_CALIBRATED_FILE,
				mpt_text("MotionPngTuberPlayer.TrackCalibratedFile"), OBS_PATH_FILE,
				"Track Files (*.json *.npz);;JSON Files (*.json);;NPZ Files (*.npz);;All Files (*.*)", NULL);

	obs_properties_add_int(props, PROP_RENDER_FPS, mpt_text("MotionPngTuberPlayer.RenderFps"), 1, 120, 1);
	obs_property_t *audio_device = obs_properties_add_list(props, PROP_AUDIO_DEVICE_IDENTITY,
						    mpt_text("MotionPngTuberPlayer.AudioDevice"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	mpt_native_populate_audio_devices(audio_device);
	obs_properties_add_int(props, PROP_AUDIO_DEVICE_INDEX, mpt_text("MotionPngTuberPlayer.AudioDeviceIndex"), -1,
			       63, 1);

	obs_property_t *policy = obs_properties_add_list(props, PROP_VALID_POLICY,
						 mpt_text("MotionPngTuberPlayer.ValidPolicy"),
						 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(policy, mpt_text("MotionPngTuberPlayer.Hold"), "hold");
	obs_property_list_add_string(policy, mpt_text("MotionPngTuberPlayer.Strict"), "strict");
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
