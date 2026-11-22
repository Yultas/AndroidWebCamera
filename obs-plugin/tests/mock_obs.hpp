#pragma once

#include <obs-module.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct MockListItem {
	std::string label;
	std::string str_val;
	long long int_val = 0;
};

struct MockProperty {
	std::string name;
	std::string description;
	std::string long_description;
	enum obs_property_type type = OBS_PROPERTY_INVALID;
	enum obs_combo_type combo_type = OBS_COMBO_TYPE_INVALID;
	enum obs_combo_format combo_format = OBS_COMBO_FORMAT_INVALID;
	enum obs_text_type text_type = OBS_TEXT_DEFAULT;
	bool visible = true;
	bool enabled = true;
	std::vector<MockListItem> items;
	obs_property_modified_t modified_cb = nullptr;
	obs_property_clicked_t clicked_cb = nullptr;
};

struct MockFrameStats {
	int count = 0;
	uint32_t last_width = 0;
	uint32_t last_height = 0;
	enum video_format last_format = VIDEO_FORMAT_NONE;
	bool last_full_range = false;
	uint64_t last_timestamp = 0;
};

namespace mock_obs {

void Reset();

bool HasRegisteredSource();
const struct obs_source_info &GetRegisteredSource();

obs_data_t *CreateData();
void DestroyData(obs_data_t *data);

obs_source_t *CreateMockSource();
void DestroyMockSource(obs_source_t *source);

void DestroyProperties(obs_properties_t *props);
const MockProperty *FindProperty(const obs_properties_t *props, const char *name);

MockFrameStats GetFrameStats();
bool WaitForFrames(int min_count, int timeout_ms);
bool WaitForDimensions(uint32_t width, uint32_t height, int timeout_ms);

} // namespace mock_obs
