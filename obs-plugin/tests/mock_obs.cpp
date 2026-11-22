#include "mock_obs.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

struct obs_data {
	std::map<std::string, std::string> strings;
	std::map<std::string, long long> ints;
	std::map<std::string, bool> bools;

	std::map<std::string, std::string> def_strings;
	std::map<std::string, long long> def_ints;
	std::map<std::string, bool> def_bools;
};

struct obs_property : public MockProperty {};

struct obs_properties {
	std::vector<std::unique_ptr<obs_property>> list;
};

struct obs_source {
	int dummy = 0;
};

namespace {

std::mutex g_obs_mutex;
std::condition_variable g_frame_cv;
bool g_has_source = false;
struct obs_source_info g_source_info{};
MockFrameStats g_frame_stats{};

obs_property_t *AddProp(obs_properties_t *props, const char *name, const char *desc,
			enum obs_property_type type)
{
	auto prop = std::make_unique<obs_property>();
	prop->name = name ? name : "";
	prop->description = desc ? desc : "";
	prop->type = type;
	obs_property_t *raw = prop.get();
	props->list.push_back(std::move(prop));
	return raw;
}

} // namespace

namespace mock_obs {

void Reset()
{
	std::lock_guard<std::mutex> lock(g_obs_mutex);
	g_has_source = false;
	std::memset(&g_source_info, 0, sizeof(g_source_info));
	g_frame_stats = MockFrameStats{};
}

bool HasRegisteredSource()
{
	std::lock_guard<std::mutex> lock(g_obs_mutex);
	return g_has_source;
}

const struct obs_source_info &GetRegisteredSource()
{
	return g_source_info;
}

obs_data_t *CreateData()
{
	return new obs_data();
}

void DestroyData(obs_data_t *data)
{
	delete data;
}

obs_source_t *CreateMockSource()
{
	return new obs_source();
}

void DestroyMockSource(obs_source_t *source)
{
	delete source;
}

void DestroyProperties(obs_properties_t *props)
{
	delete props;
}

const MockProperty *FindProperty(const obs_properties_t *props, const char *name)
{
	if (!props || !name)
		return nullptr;
	for (const auto &p : props->list) {
		if (p->name == name)
			return p.get();
	}
	return nullptr;
}

MockFrameStats GetFrameStats()
{
	std::lock_guard<std::mutex> lock(g_obs_mutex);
	return g_frame_stats;
}

bool WaitForFrames(int min_count, int timeout_ms)
{
	std::unique_lock<std::mutex> lock(g_obs_mutex);
	return g_frame_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
		return g_frame_stats.count >= min_count;
	});
}

bool WaitForDimensions(uint32_t width, uint32_t height, int timeout_ms)
{
	std::unique_lock<std::mutex> lock(g_obs_mutex);
	return g_frame_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
		return g_frame_stats.count > 0 && g_frame_stats.last_width == width &&
		       g_frame_stats.last_height == height;
	});
}

} // namespace mock_obs

extern "C" {

void blog(int log_level, const char *format, ...)
{
	(void)log_level;
	(void)format;
}

uint64_t os_gettime_ns(void)
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					     std::chrono::steady_clock::now().time_since_epoch())
					     .count());
}

void obs_register_source_s(const struct obs_source_info *info, size_t size)
{
	std::lock_guard<std::mutex> lock(g_obs_mutex);
	std::memset(&g_source_info, 0, sizeof(g_source_info));
	size_t copy_bytes = size < sizeof(g_source_info) ? size : sizeof(g_source_info);
	if (info)
		std::memcpy(&g_source_info, info, copy_bytes);
	g_has_source = true;
}

void obs_source_output_video(obs_source_t *source, const struct obs_source_frame *frame)
{
	(void)source;
	if (!frame)
		return;
	{
		std::lock_guard<std::mutex> lock(g_obs_mutex);
		g_frame_stats.count++;
		g_frame_stats.last_width = frame->width;
		g_frame_stats.last_height = frame->height;
		g_frame_stats.last_format = frame->format;
		g_frame_stats.last_full_range = frame->full_range;
		g_frame_stats.last_timestamp = frame->timestamp;
	}
	g_frame_cv.notify_all();
}

void obs_source_update_properties(obs_source_t *source)
{
	(void)source;
}

bool video_format_get_parameters_for_format(enum video_colorspace color_space,
					    enum video_range_type range,
					    enum video_format format,
					    float matrix[16],
					    float min_range[3],
					    float max_range[3])
{
	(void)color_space;
	(void)range;
	(void)format;
	if (matrix) {
		std::memset(matrix, 0, sizeof(float) * 16);
		matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
	}
	if (min_range) {
		min_range[0] = min_range[1] = min_range[2] = 0.0f;
	}
	if (max_range) {
		max_range[0] = max_range[1] = max_range[2] = 1.0f;
	}
	return true;
}

const char *obs_data_get_string(obs_data_t *data, const char *name)
{
	if (!data || !name)
		return "";
	auto it = data->strings.find(name);
	if (it != data->strings.end())
		return it->second.c_str();
	auto dit = data->def_strings.find(name);
	if (dit != data->def_strings.end())
		return dit->second.c_str();
	return "";
}

long long obs_data_get_int(obs_data_t *data, const char *name)
{
	if (!data || !name)
		return 0;
	auto it = data->ints.find(name);
	if (it != data->ints.end())
		return it->second;
	auto dit = data->def_ints.find(name);
	if (dit != data->def_ints.end())
		return dit->second;
	return 0;
}

bool obs_data_get_bool(obs_data_t *data, const char *name)
{
	if (!data || !name)
		return false;
	auto it = data->bools.find(name);
	if (it != data->bools.end())
		return it->second;
	auto dit = data->def_bools.find(name);
	if (dit != data->def_bools.end())
		return dit->second;
	return false;
}

void obs_data_set_string(obs_data_t *data, const char *name, const char *val)
{
	if (data && name)
		data->strings[name] = val ? val : "";
}

void obs_data_set_int(obs_data_t *data, const char *name, long long val)
{
	if (data && name)
		data->ints[name] = val;
}

void obs_data_set_bool(obs_data_t *data, const char *name, bool val)
{
	if (data && name)
		data->bools[name] = val;
}

void obs_data_set_default_string(obs_data_t *data, const char *name, const char *val)
{
	if (data && name)
		data->def_strings[name] = val ? val : "";
}

void obs_data_set_default_int(obs_data_t *data, const char *name, long long val)
{
	if (data && name)
		data->def_ints[name] = val;
}

void obs_data_set_default_bool(obs_data_t *data, const char *name, bool val)
{
	if (data && name)
		data->def_bools[name] = val;
}

obs_properties_t *obs_properties_create(void)
{
	return new obs_properties();
}

obs_property_t *obs_properties_get(obs_properties_t *props, const char *property)
{
	if (!props || !property)
		return nullptr;
	for (auto &p : props->list) {
		if (p->name == property)
			return p.get();
	}
	return nullptr;
}

obs_property_t *obs_properties_add_bool(obs_properties_t *props, const char *name,
					const char *description)
{
	return AddProp(props, name, description, OBS_PROPERTY_BOOL);
}

obs_property_t *obs_properties_add_int(obs_properties_t *props, const char *name,
				       const char *description, int min, int max, int step)
{
	(void)min;
	(void)max;
	(void)step;
	return AddProp(props, name, description, OBS_PROPERTY_INT);
}

obs_property_t *obs_properties_add_int_slider(obs_properties_t *props, const char *name,
					      const char *description, int min, int max, int step)
{
	(void)min;
	(void)max;
	(void)step;
	return AddProp(props, name, description, OBS_PROPERTY_INT);
}

obs_property_t *obs_properties_add_text(obs_properties_t *props, const char *name,
					const char *description, enum obs_text_type type)
{
	obs_property_t *p = AddProp(props, name, description, OBS_PROPERTY_TEXT);
	p->text_type = type;
	return p;
}

obs_property_t *obs_properties_add_list(obs_properties_t *props, const char *name,
					const char *description, enum obs_combo_type type,
					enum obs_combo_format format)
{
	obs_property_t *p = AddProp(props, name, description, OBS_PROPERTY_LIST);
	p->combo_type = type;
	p->combo_format = format;
	return p;
}

obs_property_t *obs_properties_add_button(obs_properties_t *props, const char *name,
					  const char *text, obs_property_clicked_t callback)
{
	obs_property_t *p = AddProp(props, name, text, OBS_PROPERTY_BUTTON);
	p->clicked_cb = callback;
	return p;
}

obs_property_t *obs_properties_add_group(obs_properties_t *props, const char *name,
					 const char *description, enum obs_group_type type,
					 obs_properties_t *group)
{
	(void)type;
	(void)group;
	return AddProp(props, name, description, OBS_PROPERTY_GROUP);
}

void obs_property_set_visible(obs_property_t *p, bool visible)
{
	if (p)
		p->visible = visible;
}

void obs_property_set_enabled(obs_property_t *p, bool enabled)
{
	if (p)
		p->enabled = enabled;
}

void obs_property_set_long_description(obs_property_t *p, const char *long_description)
{
	if (p)
		p->long_description = long_description ? long_description : "";
}

void obs_property_set_modified_callback(obs_property_t *p, obs_property_modified_t modified)
{
	if (p)
		p->modified_cb = modified;
}

void obs_property_list_clear(obs_property_t *p)
{
	if (p)
		p->items.clear();
}

size_t obs_property_list_add_string(obs_property_t *p, const char *name, const char *val)
{
	if (!p)
		return 0;
	MockListItem item;
	item.label = name ? name : "";
	item.str_val = val ? val : "";
	p->items.push_back(std::move(item));
	return p->items.size() - 1;
}

size_t obs_property_list_add_int(obs_property_t *p, const char *name, long long val)
{
	if (!p)
		return 0;
	MockListItem item;
	item.label = name ? name : "";
	item.int_val = val;
	p->items.push_back(std::move(item));
	return p->items.size() - 1;
}

lookup_t *obs_module_load_locale(obs_module_t *module, const char *default_locale,
				 const char *locale)
{
	(void)module;
	(void)default_locale;
	(void)locale;
	return nullptr;
}

bool text_lookup_getstr(lookup_t *lookup, const char *lookup_val, const char **out)
{
	(void)lookup;
	(void)lookup_val;
	(void)out;
	return false;
}

void text_lookup_destroy(lookup_t *lookup)
{
	(void)lookup;
}

} // extern "C"
