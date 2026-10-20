#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "json.h"

struct Preset {
    std::string name;
    std::string path;
    ojson config;
};

std::vector<Preset> load_all_presets();
const Preset* detect_preset(const std::string& filename,
                            const std::vector<std::string>& headers,
                            const std::vector<Preset>& presets);
std::map<std::string, std::string> apply_column_mapping(
    const ojson& column_mapping, const std::vector<std::string>& headers);
std::string preset_name(const Preset& preset);
