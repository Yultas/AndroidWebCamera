#pragma once

/*
 * Standalone ABI-compatible libobs header subset (OBS Studio 28-31+ C ABI).
 * Used only as a fallback when full libobs headers are not installed locally.
 * When system <obs-module.h> or OBS SDK is available, CMake prefers the official headers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define OBS_COMPAT_EXTERN_C extern "C"
extern "C" {
#else
#define OBS_COMPAT_EXTERN_C
#endif

#ifndef MAKE_SEMANTIC_VERSION
#define MAKE_SEMANTIC_VERSION(major, minor, patch) \
	(((uint32_t)(major) << 24) | ((uint32_t)(minor) << 16) | (uint32_t)(patch))
#endif

#ifndef LIBOBS_API_VER
#define LIBOBS_API_VER MAKE_SEMANTIC_VERSION(30, 0, 0)
#endif

#if defined(_WIN32)
#define MODULE_EXPORT __declspec(dllexport)
#ifndef EXPORT
#ifdef AWC_STATIC_MOCK_OBS
#define EXPORT
#else
#define EXPORT __declspec(dllimport)
#endif
#endif
#else
#define MODULE_EXPORT __attribute__((visibility("default")))
#ifndef EXPORT
#define EXPORT
#endif
#endif

#ifndef MAX_AV_PLANES
#define MAX_AV_PLANES 8
#endif

typedef struct obs_module obs_module_t;
typedef struct obs_source obs_source_t;
typedef struct obs_data obs_data_t;
typedef struct obs_properties obs_properties_t;
typedef struct obs_property obs_property_t;
typedef struct text_lookup lookup_t;
typedef struct gs_effect gs_effect_t;
typedef struct obs_missing_files obs_missing_files_t;
typedef struct obs_source_audio_mix obs_source_audio_mix;
typedef struct audio_output_data audio_output_data;
typedef struct audio_mix_data audio_mix_data;
typedef struct obs_audio_data obs_audio_data;
typedef struct obs_mouse_event obs_mouse_event;
typedef struct obs_key_event obs_key_event;

typedef void (*obs_source_enum_proc_t)(obs_source_t *parent, obs_source_t *child, void *param);

enum {
	LOG_ERROR = 100,
	LOG_WARNING = 200,
	LOG_INFO = 300,
	LOG_DEBUG = 400
};

enum obs_source_type {
	OBS_SOURCE_TYPE_INPUT,
	OBS_SOURCE_TYPE_FILTER,
	OBS_SOURCE_TYPE_TRANSITION,
	OBS_SOURCE_TYPE_SCENE
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
	OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT
};

enum obs_media_state {
	OBS_MEDIA_STATE_NONE,
	OBS_MEDIA_STATE_PLAYING,
	OBS_MEDIA_STATE_OPENING,
	OBS_MEDIA_STATE_BUFFERING,
	OBS_MEDIA_STATE_PAUSED,
	OBS_MEDIA_STATE_STOPPED,
	OBS_MEDIA_STATE_ENDED,
	OBS_MEDIA_STATE_ERROR
};

#define OBS_SOURCE_VIDEO (1 << 0)
#define OBS_SOURCE_AUDIO (1 << 1)
#define OBS_SOURCE_ASYNC (1 << 2)
#define OBS_SOURCE_ASYNC_VIDEO (OBS_SOURCE_ASYNC | OBS_SOURCE_VIDEO)
#define OBS_SOURCE_DO_NOT_DUPLICATE (1 << 7)

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
	VIDEO_FORMAT_P010
};

enum video_colorspace {
	VIDEO_CS_DEFAULT,
	VIDEO_CS_601,
	VIDEO_CS_709,
	VIDEO_CS_SRGB,
	VIDEO_CS_2100_PQ,
	VIDEO_CS_2100_HLG
};

enum video_range_type {
	VIDEO_RANGE_DEFAULT,
	VIDEO_RANGE_PARTIAL,
	VIDEO_RANGE_FULL
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

enum obs_property_type {
	OBS_PROPERTY_INVALID,
	OBS_PROPERTY_BOOL,
	OBS_PROPERTY_INT,
	OBS_PROPERTY_FLOAT,
	OBS_PROPERTY_TEXT,
	OBS_PROPERTY_PATH,
	OBS_PROPERTY_LIST,
	OBS_PROPERTY_COLOR,
	OBS_PROPERTY_BUTTON,
	OBS_PROPERTY_FONT,
	OBS_PROPERTY_EDITABLE_LIST,
	OBS_PROPERTY_FRAME_RATE,
	OBS_PROPERTY_GROUP,
	OBS_PROPERTY_COLOR_ALPHA
};

enum obs_combo_type {
	OBS_COMBO_TYPE_INVALID,
	OBS_COMBO_TYPE_EDITABLE,
	OBS_COMBO_TYPE_LIST,
	OBS_COMBO_TYPE_RADIO
};

enum obs_combo_format {
	OBS_COMBO_FORMAT_INVALID,
	OBS_COMBO_FORMAT_INT,
	OBS_COMBO_FORMAT_FLOAT,
	OBS_COMBO_FORMAT_STRING
};

enum obs_text_type {
	OBS_TEXT_DEFAULT,
	OBS_TEXT_PASSWORD,
	OBS_TEXT_MULTILINE,
	OBS_TEXT_INFO
};

enum obs_group_type {
	OBS_GROUP_INVALID,
	OBS_GROUP_NORMAL,
	OBS_GROUP_CHECKABLE
};

typedef bool (*obs_property_clicked_t)(obs_properties_t *props, obs_property_t *property, void *data);
typedef bool (*obs_property_modified_t)(obs_properties_t *props, obs_property_t *property,
					obs_data_t *settings);

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

	void (*mouse_click)(void *data, const struct obs_mouse_event *event, int32_t type,
			    bool mouse_up, uint32_t click_count);
	void (*mouse_move)(void *data, const struct obs_mouse_event *event, bool mouse_leave);
	void (*mouse_wheel)(void *data, const struct obs_mouse_event *event, int x_delta,
			    int y_delta);
	void (*focus)(void *data, bool focus);
	void (*key_click)(void *data, const struct obs_key_event *event, bool key_up);

	void (*filter_add)(void *data, obs_source_t *source);
	void (*filter_remove)(void *data, obs_source_t *source);

	void *type_data;
	void (*free_type_data)(void *type_data);

	bool (*audio_render)(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output,
			     uint32_t mixers, size_t channels, size_t sample_rate);
	void (*enum_all_sources)(void *data, obs_source_enum_proc_t enum_callback, void *param);

	void (*transition_start)(void *data);
	void (*transition_stop)(void *data);

	void (*get_defaults2)(void *type_data, obs_data_t *settings);
	obs_properties_t *(*get_properties2)(void *data, void *type_data);

	bool (*audio_mix)(void *data, uint64_t *ts_out, struct audio_output_data *audio_output,
			  size_t channels, size_t sample_rate, struct audio_mix_data *mixes);

	enum obs_icon_type icon_type;

	void (*media_play_pause)(void *data, bool pause);
	void (*media_restart)(void *data);
	void (*media_stop)(void *data);
	void (*media_next)(void *data);
	void (*media_previous)(void *data);
	int64_t (*media_get_duration)(void *data);
	int64_t (*media_get_time)(void *data);
	void (*media_set_time)(void *data, int64_t milliseconds);
	enum obs_media_state (*media_get_state)(void *data);

	uint32_t version;
	const char *unversioned_id;
};

EXPORT void blog(int log_level, const char *format, ...);
EXPORT uint64_t os_gettime_ns(void);

EXPORT void obs_register_source_s(const struct obs_source_info *info, size_t size);
#define obs_register_source(info) obs_register_source_s(info, sizeof(struct obs_source_info))

EXPORT void obs_source_output_video(obs_source_t *source, const struct obs_source_frame *frame);
EXPORT void obs_source_update_properties(obs_source_t *source);

EXPORT bool video_format_get_parameters_for_format(enum video_colorspace color_space,
						   enum video_range_type range,
						   enum video_format format,
						   float matrix[16],
						   float min_range[3],
						   float max_range[3]);

EXPORT const char *obs_data_get_string(obs_data_t *data, const char *name);
EXPORT long long obs_data_get_int(obs_data_t *data, const char *name);
EXPORT bool obs_data_get_bool(obs_data_t *data, const char *name);

EXPORT void obs_data_set_string(obs_data_t *data, const char *name, const char *val);
EXPORT void obs_data_set_int(obs_data_t *data, const char *name, long long val);
EXPORT void obs_data_set_bool(obs_data_t *data, const char *name, bool val);

EXPORT void obs_data_set_default_string(obs_data_t *data, const char *name, const char *val);
EXPORT void obs_data_set_default_int(obs_data_t *data, const char *name, long long val);
EXPORT void obs_data_set_default_bool(obs_data_t *data, const char *name, bool val);

EXPORT obs_properties_t *obs_properties_create(void);
EXPORT obs_property_t *obs_properties_get(obs_properties_t *props, const char *property);

EXPORT obs_property_t *obs_properties_add_bool(obs_properties_t *props, const char *name,
					       const char *description);
EXPORT obs_property_t *obs_properties_add_int(obs_properties_t *props, const char *name,
					      const char *description, int min, int max, int step);
EXPORT obs_property_t *obs_properties_add_int_slider(obs_properties_t *props, const char *name,
						     const char *description, int min, int max,
						     int step);
EXPORT obs_property_t *obs_properties_add_text(obs_properties_t *props, const char *name,
					       const char *description, enum obs_text_type type);
EXPORT obs_property_t *obs_properties_add_list(obs_properties_t *props, const char *name,
					       const char *description, enum obs_combo_type type,
					       enum obs_combo_format format);
EXPORT obs_property_t *obs_properties_add_button(obs_properties_t *props, const char *name,
						 const char *text,
						 obs_property_clicked_t callback);
EXPORT obs_property_t *obs_properties_add_group(obs_properties_t *props, const char *name,
						const char *description,
						enum obs_group_type type,
						obs_properties_t *group);

EXPORT void obs_property_set_visible(obs_property_t *p, bool visible);
EXPORT void obs_property_set_enabled(obs_property_t *p, bool enabled);
EXPORT void obs_property_set_long_description(obs_property_t *p, const char *long_description);
EXPORT void obs_property_set_modified_callback(obs_property_t *p, obs_property_modified_t modified);

EXPORT void obs_property_list_clear(obs_property_t *p);
EXPORT size_t obs_property_list_add_string(obs_property_t *p, const char *name, const char *val);
EXPORT size_t obs_property_list_add_int(obs_property_t *p, const char *name, long long val);

EXPORT lookup_t *obs_module_load_locale(obs_module_t *module, const char *default_locale,
					const char *locale);
EXPORT bool text_lookup_getstr(lookup_t *lookup, const char *lookup_val, const char **out);
EXPORT void text_lookup_destroy(lookup_t *lookup);

#define OBS_DECLARE_MODULE()                                                           \
	static obs_module_t *obs_module_pointer = NULL;                                \
	OBS_COMPAT_EXTERN_C MODULE_EXPORT void obs_module_set_pointer(                 \
		obs_module_t *module);                                                 \
	OBS_COMPAT_EXTERN_C void obs_module_set_pointer(obs_module_t *module)          \
	{                                                                              \
		obs_module_pointer = module;                                           \
	}                                                                              \
	OBS_COMPAT_EXTERN_C obs_module_t *obs_current_module(void);                    \
	OBS_COMPAT_EXTERN_C obs_module_t *obs_current_module(void)                     \
	{                                                                              \
		return obs_module_pointer;                                             \
	}                                                                              \
	OBS_COMPAT_EXTERN_C MODULE_EXPORT uint32_t obs_module_ver(void);               \
	OBS_COMPAT_EXTERN_C uint32_t obs_module_ver(void)                              \
	{                                                                              \
		return LIBOBS_API_VER;                                                 \
	}

#define OBS_MODULE_USE_DEFAULT_LOCALE(module_name, default_locale)                     \
	lookup_t *obs_module_lookup = NULL;                                            \
	OBS_COMPAT_EXTERN_C const char *obs_module_text(const char *val);              \
	OBS_COMPAT_EXTERN_C const char *obs_module_text(const char *val)               \
	{                                                                              \
		const char *out = val;                                                 \
		text_lookup_getstr(obs_module_lookup, val, &out);                      \
		return out;                                                            \
	}                                                                              \
	OBS_COMPAT_EXTERN_C bool obs_module_get_string(const char *val,                \
						       const char **out);              \
	OBS_COMPAT_EXTERN_C bool obs_module_get_string(const char *val,                \
						       const char **out)               \
	{                                                                              \
		return text_lookup_getstr(obs_module_lookup, val, out);                \
	}                                                                              \
	OBS_COMPAT_EXTERN_C MODULE_EXPORT void obs_module_set_locale(                  \
		const char *locale);                                                   \
	OBS_COMPAT_EXTERN_C void obs_module_set_locale(const char *locale)             \
	{                                                                              \
		if (obs_module_lookup)                                                 \
			text_lookup_destroy(obs_module_lookup);                        \
		obs_module_lookup = obs_module_load_locale(obs_current_module(),       \
							   default_locale, locale);    \
	}                                                                              \
	OBS_COMPAT_EXTERN_C MODULE_EXPORT void obs_module_free_locale(void);           \
	OBS_COMPAT_EXTERN_C void obs_module_free_locale(void)                          \
	{                                                                              \
		text_lookup_destroy(obs_module_lookup);                                \
		obs_module_lookup = NULL;                                              \
	}

MODULE_EXPORT bool obs_module_load(void);
MODULE_EXPORT void obs_module_unload(void);
MODULE_EXPORT const char *obs_module_name(void);
MODULE_EXPORT const char *obs_module_description(void);

#ifdef __cplusplus
}
#endif
