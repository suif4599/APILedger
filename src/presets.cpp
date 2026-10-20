#include "presets.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>

#include "paths.h"
#include "util.h"

namespace fs = std::filesystem;

namespace {

bool has_key(const ojson& config, const std::string& key) {
    return config.is_object() && config.contains(key);
}

std::vector<std::string> string_list(const ojson& value) {
    std::vector<std::string> out;
    if (!value.is_array()) return out;
    for (const auto& item : value) {
        if (item.is_string()) out.push_back(item.get<std::string>());
    }
    return out;
}

std::vector<std::string> preset_files(const std::string& directory) {
    std::vector<std::string> files;
    if (directory.empty() || !fs::is_directory(directory)) return files;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '_' || name[0] == '.') continue;
        if (!util::ends_with_ci(name, ".json")) continue;
        files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace

std::string preset_name(const Preset& preset) {
    return preset.name;
}

std::vector<Preset> load_all_presets() {
    std::vector<Preset> presets;
    for (const std::string& directory : paths::layout().preset_dirs) {
        for (const std::string& path : preset_files(directory)) {
            const std::string filename = util::basename(path);
            ojson config;
            try {
                config = ojson::parse(util::read_file(path));
            } catch (const std::exception& error) {
                std::cout << "  [preset] failed to load " << filename << ": " << error.what() << "\n";
                continue;
            }
            if (!config.is_object() || !has_key(config, "match_filename") ||
                !has_key(config, "column_mapping")) {
                std::cout << "  [preset] " << filename
                          << " is missing required fields (match_filename / column_mapping),"
                             " skipped\n";
                continue;
            }

            const std::string name =
                config.contains("name") && config["name"].is_string()
                    ? config["name"].get<std::string>()
                    : util::stem(path);
            Preset preset{name, path, std::move(config)};
            auto existing = std::find_if(presets.begin(), presets.end(),
                                         [&](const Preset& p) { return p.name == name; });
            if (existing != presets.end()) {
                *existing = std::move(preset);
            } else {
                presets.push_back(std::move(preset));
            }
        }
    }
    return presets;
}

const Preset* detect_preset(const std::string& filename,
                            const std::vector<std::string>& headers,
                            const std::vector<Preset>& presets) {
    if (presets.empty()) return nullptr;

    const std::string name_norm = util::normalize_key(filename);
    std::vector<std::string> header_norm;
    header_norm.reserve(headers.size());
    for (const std::string& header : headers) {
        header_norm.push_back(util::normalize_key(header));
    }

    const Preset* best = nullptr;
    int best_hits = -1;

    for (const Preset& preset : presets) {
        const auto filename_keywords =
            string_list(preset.config.value("match_filename", ojson::array()));
        if (!filename_keywords.empty()) {
            bool found = false;
            for (const std::string& keyword : filename_keywords) {
                if (name_norm.find(util::normalize_key(keyword)) != std::string::npos) {
                    found = true;
                    break;
                }
            }
            if (!found) continue;
        }

        const auto header_keywords =
            string_list(preset.config.value("match_headers", ojson::array()));
        int hits = 0;
        if (!header_keywords.empty()) {
            for (const std::string& keyword : header_keywords) {
                const std::string keyword_norm = util::normalize_key(keyword);
                for (const std::string& header : header_norm) {
                    if (keyword_norm == header ||
                        header.find(keyword_norm) != std::string::npos) {
                        ++hits;
                        break;
                    }
                }
            }
            if (hits != static_cast<int>(header_keywords.size())) continue;
        }

        if (hits > best_hits) {
            best = &preset;
            best_hits = hits;
        }
    }
    return best;
}

std::map<std::string, std::string> apply_column_mapping(
    const ojson& column_mapping, const std::vector<std::string>& headers) {
    struct Keyword {
        std::string keyword;
        std::string field;
    };

    std::vector<Keyword> flat;
    if (column_mapping.is_object()) {
        for (auto it = column_mapping.begin(); it != column_mapping.end(); ++it) {
            for (const std::string& keyword : string_list(it.value())) {
                flat.push_back({keyword, it.key()});
            }
        }
    }

    std::stable_sort(flat.begin(), flat.end(), [](const Keyword& a, const Keyword& b) {
        return a.keyword.size() > b.keyword.size();
    });

    std::map<std::string, std::string> matched;
    std::set<std::string> used_headers;
    std::set<std::string> used_fields;

    for (const Keyword& entry : flat) {
        if (used_fields.count(entry.field)) continue;
        const std::string keyword_norm = util::normalize_key(entry.keyword);
        for (const std::string& header : headers) {
            if (used_headers.count(header)) continue;
            if (util::normalize_key(header).find(keyword_norm) != std::string::npos) {
                matched[entry.field] = header;
                used_headers.insert(header);
                used_fields.insert(entry.field);
                break;
            }
        }
    }
    return matched;
}

