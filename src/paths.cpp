#include "paths.h"

#include <cstdlib>
#include <string>
#include <vector>

#ifndef APILEDGER_DEFAULT_SHARE_DIR
#define APILEDGER_DEFAULT_SHARE_DIR "/usr/local/share/apiledger"
#endif

namespace paths {

namespace {

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? std::string(value) : std::string();
}

Layout build_layout() {
    Layout layout;
    layout.root = env_or_empty("APILEDGER_ROOT");
    if (layout.root.empty()) {
        const std::string xdg = env_or_empty("XDG_DATA_HOME");
        if (!xdg.empty()) {
            layout.root = xdg + "/apiledger";
        } else {
            layout.root = env_or_empty("HOME") + "/.local/share/apiledger";
        }
    }

    layout.data_dir = layout.root + "/data";
    layout.db_path = layout.data_dir + "/api_ledger.db";
    layout.report_path = layout.data_dir + "/report.html";
    layout.cache_dir = layout.root + "/.cache";
    layout.echarts_path = layout.cache_dir + "/echarts.min.js";

    layout.share_dir = env_or_empty("APILEDGER_SHARE_DIR");
    if (layout.share_dir.empty()) {
        layout.share_dir = APILEDGER_DEFAULT_SHARE_DIR;
    }
    layout.presets_dir = layout.share_dir + "/presets";
    layout.preset_dirs = {layout.presets_dir, layout.root + "/presets"};
    layout.template_path = layout.share_dir + "/report_template.html";
    return layout;
}

Layout g_layout;

}  // namespace

const Layout& layout() { return g_layout; }

void init() { g_layout = build_layout(); }

}  // namespace paths
