#include "report.h"

#include <curl/curl.h>

#include <filesystem>
#include <iostream>

#include "paths.h"
#include "util.h"

namespace fs = std::filesystem;

namespace {

const char* ECHARTS_CDN = "https://cdn.jsdelivr.net/npm/echarts@5/dist/echarts.min.js";

size_t curl_write(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

bool download_file(const std::string& url, const std::string& path) {
    static const bool initialized = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)initialized;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "APILedger");

    const CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) return false;

    util::write_file(path, body);
    return true;
}

std::string echarts_script_tag() {
    const paths::Layout& layout = paths::layout();
    if (util::file_exists(layout.echarts_path)) {
        return "<script>\n" + util::read_file(layout.echarts_path) + "\n</script>";
    }

    try {
        util::make_dirs(layout.cache_dir);
        std::cout << "  [report] downloading echarts.min.js (first use)...\n";
        if (download_file(ECHARTS_CDN, layout.echarts_path)) {
            return "<script>\n" + util::read_file(layout.echarts_path) + "\n</script>";
        }
        std::cout << "  [report] echarts download failed, falling back to CDN\n";
    } catch (const std::exception& error) {
        std::cout << "  [report] echarts download failed, falling back to CDN: "
                  << error.what() << "\n";
    }
    return std::string("<script src=\"") + ECHARTS_CDN + "\"></script>";
}

}  // namespace

std::string build_html(Database& db) {
    const ojson data = build_report_data(db);
    const std::string echarts_tag = echarts_script_tag();
    const std::string report_js = util::read_file(paths::layout().report_js_path);

    std::string html = util::read_file(paths::layout().template_path);
    html = util::replace_all(html, "__DATA_JSON__", data.dump());
    html = util::replace_all(html, "__REPORT_JS__", report_js);
    html = util::replace_all(html, "__ECHARTS_TAG__", echarts_tag);
    return html;
}

std::string export_report(Database& db, const std::string& output_path) {
    std::string path = output_path;
    if (path.empty()) {
        util::make_dirs(paths::layout().data_dir);
        path = paths::layout().report_path;
    }
    util::write_file(path, build_html(db));

    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    return ec ? path : absolute.string();
}

