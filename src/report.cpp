#include "report.h"

#include <curl/curl.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>

#include "paths.h"
#include "util.h"

namespace fs = std::filesystem;

namespace {

const char* ECHARTS_CDN = "https://cdn.jsdelivr.net/npm/echarts@5/dist/echarts.min.js";

const std::vector<std::string> CHART_COLORS = {
    "#1f538d", "#2fa572", "#e8a838", "#c0392b",
    "#8e44ad", "#16a085", "#2980b9", "#d35400",
    "#27ae60", "#f39c12", "#7f8c8d", "#2c3e50",
};

const std::vector<std::string> CHART_COLORS_DARK = {
    "#5b9bd5", "#3fbf8a", "#f0b445", "#e5736a",
    "#b07fd8", "#2ec4a6", "#4aa8e0", "#e88a4a",
    "#4dd18a", "#f5c542", "#9aa5b1", "#a8b8cc",
};

double round_value(double value, int decimals) {
    const double factor = std::pow(10.0, decimals);
    return std::round(value * factor) / factor;
}

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

ojson build_report_data(Database& db) {
    const std::vector<StoredRecord> records = db.get_all("date DESC");

    ojson summary = ojson::object();
    summary["record_count"] = static_cast<long long>(records.size());
    long long total_tokens = 0;
    double total_cost = 0.0;
    std::set<std::string> models;
    std::set<std::string> platforms;
    for (const StoredRecord& record : records) {
        total_tokens += record.tokens;
        total_cost += record.cost;
        if (!record.model.empty()) models.insert(record.model);
        if (!record.platform.empty()) platforms.insert(record.platform);
    }
    summary["total_tokens"] = total_tokens;
    summary["total_cost"] = round_value(total_cost, 2);
    summary["model_count"] = static_cast<long long>(models.size());
    summary["platform_count"] = static_cast<long long>(platforms.size());
    if (!records.empty()) {
        summary["date_min"] = records.back().date.substr(0, 10);
        summary["date_max"] = records.front().date.substr(0, 10);
    } else {
        summary["date_min"] = "";
        summary["date_max"] = "";
    }

    ojson raw_records = ojson::array();
    for (const StoredRecord& record : records) {
        ojson item = ojson::object();
        item["bill_start"] = record.date;
        item["platform"] = record.platform;
        item["project"] = record.project;
        item["model"] = record.model;
        item["type"] = record.type;
        item["tokens"] = record.tokens;
        item["cost"] = record.cost;
        item["unit_price"] = record.unit_price;
        item["source_file"] = record.source_file;
        raw_records.push_back(std::move(item));
    }

    ojson data = ojson::object();
    data["summary"] = summary;
    data["records"] = raw_records;
    return data;
}

std::string build_html(Database& db) {
    const ojson data = build_report_data(db);
    const std::string echarts_tag = echarts_script_tag();

    std::string html = util::read_file(paths::layout().template_path);
    html = util::replace_all(html, "__DATA_JSON__", data.dump());
    html = util::replace_all(html, "__CHART_COLORS__", ojson(CHART_COLORS).dump());
    html = util::replace_all(html, "__CHART_COLORS_DARK__", ojson(CHART_COLORS_DARK).dump());
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

