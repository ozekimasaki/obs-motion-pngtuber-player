#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UNUSED_PARAMETER(param) (void)(param)

#ifdef _MSC_VER
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

#define LIBOBS_API_MAJOR_VER 32
#define LIBOBS_API_MINOR_VER 0
#define LIBOBS_API_PATCH_VER 4
#define MAKE_SEMANTIC_VERSION(major, minor, patch) ((uint32_t)(((major) << 24) | ((minor) << 16) | (patch)))
#define LIBOBS_API_VER MAKE_SEMANTIC_VERSION(LIBOBS_API_MAJOR_VER, LIBOBS_API_MINOR_VER, LIBOBS_API_PATCH_VER)

#define MAX_AV_PLANES 8
#define MAX_AUDIO_MIXES 6
#define MAX_AUDIO_CHANNELS 8

enum {
	LOG_ERROR = 100,
	LOG_WARNING = 200,
	LOG_INFO = 300,
	LOG_DEBUG = 400,
};

#ifdef __cplusplus
extern "C" {
#endif

struct obs_source;
struct obs_module;
struct obs_data;
struct obs_data_array;
struct obs_properties;
struct obs_property;
struct signal_handler;
struct calldata;
struct text_lookup;
struct gs_effect;
struct obs_missing_files;
struct obs_audio_data;
struct obs_mouse_event;
struct obs_key_event;
struct obs_source_audio_mix;
struct audio_output_data;
struct audio_output_info;
struct audio_data;
struct audio_output;

typedef struct obs_source obs_source_t;
typedef struct obs_module obs_module_t;
typedef struct obs_data obs_data_t;
typedef struct obs_data_array obs_data_array_t;
typedef struct obs_properties obs_properties_t;
typedef struct obs_property obs_property_t;
typedef struct signal_handler signal_handler_t;
typedef struct calldata calldata_t;
typedef struct text_lookup lookup_t;
typedef struct gs_effect gs_effect_t;
typedef struct obs_missing_files obs_missing_files_t;
typedef struct audio_output audio_t;

enum gs_color_space {
	GS_CS_UNKNOWN = 0,
};

enum video_format {
	VIDEO_FORMAT_NONE,
	VIDEO_FORMAT_I420,
	VIDEO_FORMAT_NV12,
	VIDEO_FORMAT_YVYU,
	VIDEO_FORMAT_YUY2,
	VIDEO_FORMAT_UYVY,
	VIDEO_FORMAT_RGBA,
	VIDEO_FORMAT_BGRA,
	VIDEO_FORMAT_BGRX,
	VIDEO_FORMAT_Y800,
	VIDEO_FORMAT_I444,
	VIDEO_FORMAT_BGR3,
	VIDEO_FORMAT_I422,
	VIDEO_FORMAT_I40A,
	VIDEO_FORMAT_I42A,
	VIDEO_FORMAT_YUVA,
	VIDEO_FORMAT_AYUV,
	VIDEO_FORMAT_I010,
	VIDEO_FORMAT_P010,
	VIDEO_FORMAT_I210,
	VIDEO_FORMAT_I412,
	VIDEO_FORMAT_YA2L,
	VIDEO_FORMAT_P216,
	VIDEO_FORMAT_P416,
	VIDEO_FORMAT_V210,
	VIDEO_FORMAT_R10L,
};

enum audio_format {
	AUDIO_FORMAT_UNKNOWN,
	AUDIO_FORMAT_U8BIT,
	AUDIO_FORMAT_16BIT,
	AUDIO_FORMAT_32BIT,
	AUDIO_FORMAT_FLOAT,
	AUDIO_FORMAT_U8BIT_PLANAR,
	AUDIO_FORMAT_16BIT_PLANAR,
	AUDIO_FORMAT_32BIT_PLANAR,
	AUDIO_FORMAT_FLOAT_PLANAR,
};

enum speaker_layout {
	SPEAKERS_UNKNOWN,
	SPEAKERS_MONO,
	SPEAKERS_STEREO,
	SPEAKERS_2POINT1,
	SPEAKERS_4POINT0,
	SPEAKERS_4POINT1,
	SPEAKERS_5POINT1,
	SPEAKERS_7POINT1 = 8,
};

struct obs_source_frame {
	uint8_t *data[MAX_AV_PLANES];
	uint32_t linesize[MAX_AV_PLANES];
	uint32_t width;
	uint32_t height;
	uint64_t timestamp;
	enum video_format format;
	float color_matrix[16];
	bool full_range;
	uint16_t max_luminance;
	float color_range_min[3];
	float color_range_max[3];
	bool flip;
	uint8_t flags;
	uint8_t trc;
	volatile long refs;
	bool prev_frame;
};

struct audio_data {
	uint8_t *data[MAX_AV_PLANES];
	uint32_t frames;
	uint64_t timestamp;
};

struct audio_output_data {
	float *data[MAX_AUDIO_CHANNELS];
};

typedef bool (*audio_input_callback_t)(void *param, uint64_t start_ts, uint64_t end_ts, uint64_t *new_ts,
				       uint32_t active_mixers, struct audio_output_data *mixes);

struct audio_output_info {
	const char *name;
	uint32_t samples_per_sec;
	enum audio_format format;
	enum speaker_layout speakers;
	audio_input_callback_t input_callback;
	void *input_param;
};

enum obs_source_type {
	OBS_SOURCE_TYPE_INPUT,
	OBS_SOURCE_TYPE_FILTER,
	OBS_SOURCE_TYPE_TRANSITION,
	OBS_SOURCE_TYPE_SCENE,
};

enum obs_icon_type {
	OBS_ICON_TYPE_UNKNOWN,
	OBS_ICON_TYPE_IMAGE,
	OBS_ICON_TYPE_COLOR,
	OBS_ICON_TYPE_SLIDESHOW,
	OBS_ICON_TYPE_AUDIO_INPUT,
	OBS_ICON_TYPE_AUDIO_OUTPUT,
	OBS_ICON_TYPE_DESKTOP_CAPTURE,
	OBS_ICON_TYPE_WINDOW_CAPTURE,
	OBS_ICON_TYPE_GAME_CAPTURE,
	OBS_ICON_TYPE_CAMERA,
	OBS_ICON_TYPE_TEXT,
	OBS_ICON_TYPE_MEDIA,
	OBS_ICON_TYPE_BROWSER,
	OBS_ICON_TYPE_CUSTOM,
	OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT,
};

enum obs_media_state {
	OBS_MEDIA_STATE_NONE,
	OBS_MEDIA_STATE_PLAYING,
	OBS_MEDIA_STATE_OPENING,
	OBS_MEDIA_STATE_BUFFERING,
	OBS_MEDIA_STATE_PAUSED,
	OBS_MEDIA_STATE_STOPPED,
	OBS_MEDIA_STATE_ENDED,
	OBS_MEDIA_STATE_ERROR,
};

#define OBS_SOURCE_VIDEO (1 << 0)
#define OBS_SOURCE_AUDIO (1 << 1)
#define OBS_SOURCE_ASYNC (1 << 2)
#define OBS_SOURCE_ASYNC_VIDEO (OBS_SOURCE_ASYNC | OBS_SOURCE_VIDEO)
#define OBS_SOURCE_CUSTOM_DRAW (1 << 3)
#define OBS_SOURCE_INTERACTION (1 << 5)
#define OBS_SOURCE_COMPOSITE (1 << 6)
#define OBS_SOURCE_DO_NOT_DUPLICATE (1 << 7)
#define OBS_SOURCE_DEPRECATED (1 << 8)
#define OBS_SOURCE_DO_NOT_SELF_MONITOR (1 << 9)
#define OBS_SOURCE_CAP_DISABLED (1 << 10)
#define OBS_SOURCE_CAP_OBSOLETE OBS_SOURCE_CAP_DISABLED
#define OBS_SOURCE_MONITOR_BY_DEFAULT (1 << 11)
#define OBS_SOURCE_SUBMIX (1 << 12)
#define OBS_SOURCE_CONTROLLABLE_MEDIA (1 << 13)
#define OBS_SOURCE_CEA_708 (1 << 14)
#define OBS_SOURCE_SRGB (1 << 15)
#define OBS_SOURCE_CAP_DONT_SHOW_PROPERTIES (1 << 16)
#define OBS_SOURCE_REQUIRES_CANVAS (1 << 17)

typedef void (*obs_source_enum_proc_t)(obs_source_t *parent, obs_source_t *child, void *param);
typedef void (*obs_source_audio_capture_t)(void *param, obs_source_t *source, const struct audio_data *audio_data,
					   bool muted);

static inline uint32_t get_audio_channels(enum speaker_layout speakers)
{
	switch (speakers) {
	case SPEAKERS_MONO:
		return 1;
	case SPEAKERS_STEREO:
		return 2;
	case SPEAKERS_2POINT1:
		return 3;
	case SPEAKERS_4POINT0:
		return 4;
	case SPEAKERS_4POINT1:
		return 5;
	case SPEAKERS_5POINT1:
		return 6;
	case SPEAKERS_7POINT1:
		return 8;
	case SPEAKERS_UNKNOWN:
		return 0;
	}

	return 0;
}

static inline size_t get_audio_bytes_per_channel(enum audio_format format)
{
	switch (format) {
	case AUDIO_FORMAT_U8BIT:
	case AUDIO_FORMAT_U8BIT_PLANAR:
		return 1;

	case AUDIO_FORMAT_16BIT:
	case AUDIO_FORMAT_16BIT_PLANAR:
		return 2;

	case AUDIO_FORMAT_FLOAT:
	case AUDIO_FORMAT_FLOAT_PLANAR:
	case AUDIO_FORMAT_32BIT:
	case AUDIO_FORMAT_32BIT_PLANAR:
		return 4;

	case AUDIO_FORMAT_UNKNOWN:
		return 0;
	}

	return 0;
}

static inline bool is_audio_planar(enum audio_format format)
{
	switch (format) {
	case AUDIO_FORMAT_U8BIT:
	case AUDIO_FORMAT_16BIT:
	case AUDIO_FORMAT_32BIT:
	case AUDIO_FORMAT_FLOAT:
		return false;

	case AUDIO_FORMAT_U8BIT_PLANAR:
	case AUDIO_FORMAT_16BIT_PLANAR:
	case AUDIO_FORMAT_32BIT_PLANAR:
	case AUDIO_FORMAT_FLOAT_PLANAR:
		return true;

	case AUDIO_FORMAT_UNKNOWN:
		return false;
	}

	return false;
}

static inline size_t get_audio_planes(enum audio_format format, enum speaker_layout speakers)
{
	return is_audio_planar(format) ? get_audio_channels(speakers) : 1;
}

static inline uint64_t audio_frames_to_ns(size_t sample_rate, uint64_t frames)
{
	return sample_rate > 0 ? (frames * 1000000000ULL) / (uint64_t)sample_rate : 0ULL;
}

struct obs_source_info {
	const char *id;
	enum obs_source_type type;
	uint32_t output_flags;
	const char *(*get_name)(void *type_data);
	void *(*create)(obs_data_t *settings, obs_source_t *source);
	void (*destroy)(void *data);
	uint32_t (*get_width)(void *data);
	uint32_t (*get_height)(void *data);
	void (*get_defaults)(obs_data_t *settings);
	obs_properties_t *(*get_properties)(void *data);
	void (*update)(void *data, obs_data_t *settings);
	void (*activate)(void *data);
	void (*deactivate)(void *data);
	void (*show)(void *data);
	void (*hide)(void *data);
	void (*video_tick)(void *data, float seconds);
	void (*video_render)(void *data, gs_effect_t *effect);
	struct obs_source_frame *(*filter_video)(void *data, struct obs_source_frame *frame);
	struct obs_audio_data *(*filter_audio)(void *data, struct obs_audio_data *audio);
	void (*enum_active_sources)(void *data, obs_source_enum_proc_t enum_callback, void *param);
	void (*save)(void *data, obs_data_t *settings);
	void (*load)(void *data, obs_data_t *settings);
	void (*mouse_click)(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
			    uint32_t click_count);
	void (*mouse_move)(void *data, const struct obs_mouse_event *event, bool mouse_leave);
	void (*mouse_wheel)(void *data, const struct obs_mouse_event *event, int x_delta, int y_delta);
	void (*focus)(void *data, bool focus);
	void (*key_click)(void *data, const struct obs_key_event *event, bool key_up);
	void (*filter_remove)(void *data, obs_source_t *source);
	void *type_data;
	void (*free_type_data)(void *type_data);
	bool (*audio_render)(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output, uint32_t mixers,
			     size_t channels, size_t sample_rate);
	void (*enum_all_sources)(void *data, obs_source_enum_proc_t enum_callback, void *param);
	void (*transition_start)(void *data);
	void (*transition_stop)(void *data);
	void (*get_defaults2)(void *type_data, obs_data_t *settings);
	obs_properties_t *(*get_properties2)(void *data, void *type_data);
	bool (*audio_mix)(void *data, uint64_t *ts_out, struct audio_output_data *audio_output, size_t channels,
			  size_t sample_rate);
	enum obs_icon_type icon_type;
	void (*media_play_pause)(void *data, bool pause);
	void (*media_restart)(void *data);
	void (*media_stop)(void *data);
	void (*media_next)(void *data);
	void (*media_previous)(void *data);
	int64_t (*media_get_duration)(void *data);
	int64_t (*media_get_time)(void *data);
	void (*media_set_time)(void *data, int64_t miliseconds);
	enum obs_media_state (*media_get_state)(void *data);
	uint32_t version;
	const char *unversioned_id;
	obs_missing_files_t *(*missing_files)(void *data);
	enum gs_color_space (*video_get_color_space)(void *data, size_t count,
						     const enum gs_color_space *preferred_spaces);
	void (*filter_add)(void *data, obs_source_t *source);
};

enum obs_combo_format {
	OBS_COMBO_FORMAT_INVALID,
	OBS_COMBO_FORMAT_INT,
	OBS_COMBO_FORMAT_FLOAT,
	OBS_COMBO_FORMAT_STRING,
	OBS_COMBO_FORMAT_BOOL,
};

enum obs_combo_type {
	OBS_COMBO_TYPE_INVALID,
	OBS_COMBO_TYPE_EDITABLE,
	OBS_COMBO_TYPE_LIST,
	OBS_COMBO_TYPE_RADIO,
};

enum obs_path_type {
	OBS_PATH_FILE,
	OBS_PATH_FILE_SAVE,
	OBS_PATH_DIRECTORY,
};

enum obs_text_type {
	OBS_TEXT_DEFAULT,
	OBS_TEXT_PASSWORD,
	OBS_TEXT_MULTILINE,
	OBS_TEXT_INFO,
};

#define OBS_PROPERTIES_DEFER_UPDATE (1 << 0)

typedef bool (*obs_property_modified_t)(obs_properties_t *props, obs_property_t *property, obs_data_t *settings);
typedef void (*signal_callback_t)(void *data, calldata_t *cd);

EXPORT void obs_register_source_s(const struct obs_source_info *info, size_t size);
EXPORT audio_t *obs_get_audio(void);
EXPORT const struct audio_output_info *audio_output_get_info(const audio_t *audio);
EXPORT void obs_enum_sources(bool (*enum_proc)(void *, obs_source_t *), void *param);
EXPORT obs_source_t *obs_get_source_by_uuid(const char *uuid);
EXPORT void obs_source_release(obs_source_t *source);
EXPORT const char *obs_source_get_id(const obs_source_t *source);
EXPORT const char *obs_source_get_name(const obs_source_t *source);
EXPORT const char *obs_source_get_unversioned_id(const obs_source_t *source);
EXPORT const char *obs_source_get_uuid(const obs_source_t *source);
EXPORT uint32_t obs_source_get_output_flags(const obs_source_t *source);
EXPORT signal_handler_t *obs_source_get_signal_handler(const obs_source_t *source);
EXPORT void obs_source_add_audio_capture_callback(obs_source_t *source, obs_source_audio_capture_t callback,
						  void *param);
EXPORT void obs_source_remove_audio_capture_callback(obs_source_t *source, obs_source_audio_capture_t callback,
						     void *param);
EXPORT void obs_source_output_video(obs_source_t *source, const struct obs_source_frame *frame);
EXPORT void signal_handler_connect(signal_handler_t *handler, const char *signal, signal_callback_t callback, void *data);
EXPORT void signal_handler_disconnect(signal_handler_t *handler, const char *signal, signal_callback_t callback,
				      void *data);

EXPORT obs_data_t *obs_data_create(void);
EXPORT obs_data_t *obs_data_create_from_json(const char *json_string);
EXPORT obs_data_t *obs_data_create_from_json_file(const char *json_file);
EXPORT void obs_data_release(obs_data_t *data);
EXPORT const char *obs_data_get_json(obs_data_t *data);
EXPORT void obs_data_set_string(obs_data_t *data, const char *name, const char *val);
EXPORT void obs_data_set_int(obs_data_t *data, const char *name, long long val);
EXPORT void obs_data_set_double(obs_data_t *data, const char *name, double val);
EXPORT void obs_data_set_bool(obs_data_t *data, const char *name, bool val);
EXPORT void obs_data_set_obj(obs_data_t *data, const char *name, obs_data_t *obj);
EXPORT void obs_data_set_default_string(obs_data_t *data, const char *name, const char *val);
EXPORT void obs_data_set_default_int(obs_data_t *data, const char *name, long long val);
EXPORT void obs_data_set_default_bool(obs_data_t *data, const char *name, bool val);
EXPORT const char *obs_data_get_string(obs_data_t *data, const char *name);
EXPORT long long obs_data_get_int(obs_data_t *data, const char *name);
EXPORT double obs_data_get_double(obs_data_t *data, const char *name);
EXPORT bool obs_data_get_bool(obs_data_t *data, const char *name);
EXPORT obs_data_t *obs_data_get_obj(obs_data_t *data, const char *name);
EXPORT obs_data_array_t *obs_data_get_array(obs_data_t *data, const char *name);
EXPORT bool obs_data_save_json_safe(obs_data_t *data, const char *file, const char *temp_ext, const char *backup_ext);
EXPORT void obs_data_array_release(obs_data_array_t *array);
EXPORT size_t obs_data_array_count(obs_data_array_t *array);
EXPORT obs_data_t *obs_data_array_item(obs_data_array_t *array, size_t idx);

EXPORT obs_properties_t *obs_properties_create(void);
EXPORT void obs_properties_set_flags(obs_properties_t *props, uint32_t flags);
EXPORT obs_property_t *obs_properties_get(obs_properties_t *props, const char *property);
EXPORT obs_property_t *obs_properties_add_text(obs_properties_t *props, const char *name, const char *description,
					       enum obs_text_type type);
EXPORT obs_property_t *obs_properties_add_path(obs_properties_t *props, const char *name, const char *description,
					       enum obs_path_type type, const char *filter, const char *default_path);
EXPORT obs_property_t *obs_properties_add_int(obs_properties_t *props, const char *name, const char *description,
					      int min, int max, int step);
EXPORT obs_property_t *obs_properties_add_list(obs_properties_t *props, const char *name, const char *description,
					       enum obs_combo_type type, enum obs_combo_format format);
EXPORT obs_property_t *obs_properties_add_bool(obs_properties_t *props, const char *name, const char *description);
EXPORT void obs_property_set_modified_callback(obs_property_t *p, obs_property_modified_t modified);
EXPORT void obs_property_set_visible(obs_property_t *p, bool visible);
EXPORT void obs_property_list_clear(obs_property_t *p);
EXPORT size_t obs_property_list_add_string(obs_property_t *p, const char *name, const char *val);
EXPORT void obs_property_list_item_disable(obs_property_t *p, size_t idx, bool disabled);

EXPORT lookup_t *obs_module_load_locale(obs_module_t *module, const char *default_locale, const char *locale);
EXPORT bool text_lookup_getstr(lookup_t *lookup, const char *lookup_val, const char **out);
EXPORT void text_lookup_destroy(lookup_t *lookup);
EXPORT char *obs_find_module_file(obs_module_t *module, const char *file);
EXPORT char *obs_module_get_config_path(obs_module_t *module, const char *file);
EXPORT const char *obs_get_locale(void);

#define obs_register_source(info) obs_register_source_s((info), sizeof(struct obs_source_info))

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#define MODULE_EXPORT extern "C" EXPORT
#define MODULE_EXTERN extern "C"
#else
#define MODULE_EXPORT EXPORT
#define MODULE_EXTERN extern
#endif

#define OBS_DECLARE_MODULE()                                             \
	static obs_module_t *obs_module_pointer;                         \
	MODULE_EXPORT void obs_module_set_pointer(obs_module_t *module); \
	void obs_module_set_pointer(obs_module_t *module)                \
	{                                                                \
		obs_module_pointer = module;                             \
	}                                                                \
	obs_module_t *obs_current_module(void)                           \
	{                                                                \
		return obs_module_pointer;                               \
	}                                                                \
	MODULE_EXPORT uint32_t obs_module_ver(void);                     \
	uint32_t obs_module_ver(void)                                    \
	{                                                                \
		return LIBOBS_API_VER;                                   \
	}

#define OBS_MODULE_USE_DEFAULT_LOCALE(module_name, default_locale)                                        \
	lookup_t *obs_module_lookup = NULL;                                                               \
	const char *obs_module_text(const char *val)                                                      \
	{                                                                                                 \
		const char *out = val;                                                                    \
		text_lookup_getstr(obs_module_lookup, val, &out);                                         \
		return out;                                                                               \
	}                                                                                                 \
	bool obs_module_get_string(const char *val, const char **out)                                     \
	{                                                                                                 \
		return text_lookup_getstr(obs_module_lookup, val, out);                                   \
	}                                                                                                 \
	void obs_module_set_locale(const char *locale)                                                    \
	{                                                                                                 \
		if (obs_module_lookup)                                                                    \
			text_lookup_destroy(obs_module_lookup);                                           \
		obs_module_lookup = obs_module_load_locale(obs_current_module(), default_locale, locale); \
	}                                                                                                 \
	void obs_module_free_locale(void)                                                                 \
	{                                                                                                 \
		if (obs_module_lookup)                                                                    \
			text_lookup_destroy(obs_module_lookup);                                           \
		obs_module_lookup = NULL;                                                                 \
	}

MODULE_EXPORT bool obs_module_load(void);
MODULE_EXPORT void obs_module_unload(void);
MODULE_EXPORT void obs_module_post_load(void);
MODULE_EXPORT void obs_module_set_locale(const char *locale);
MODULE_EXPORT void obs_module_free_locale(void);
MODULE_EXPORT bool obs_module_get_string(const char *lookup_string, const char **translated_string);
MODULE_EXPORT const char *obs_module_name(void);
MODULE_EXPORT const char *obs_module_description(void);
MODULE_EXTERN const char *obs_module_text(const char *lookup_string);
MODULE_EXTERN obs_module_t *obs_current_module(void);

#define obs_module_file(file) obs_find_module_file(obs_current_module(), (file))
#define obs_module_config_path(file) obs_module_get_config_path(obs_current_module(), (file))
