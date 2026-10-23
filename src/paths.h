#pragma once

#include <string>
#include <vector>

namespace paths {

struct Layout {
    std::string root;
    std::string data_dir;
    std::string db_path;
    std::string report_path;
    std::string cache_dir;
    std::string echarts_path;
    std::string share_dir;
    std::string presets_dir;
    std::vector<std::string> preset_dirs;
    std::string template_path;
    std::string report_js_path;
};

const Layout& layout();
void init();

}
