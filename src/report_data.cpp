#include "report.h"

#include <cmath>
#include <set>

namespace {

double round_value(double value, int decimals) {
    const double factor = std::pow(10.0, decimals);
    return std::round(value * factor) / factor;
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
        item["currency"] = record.currency;
        raw_records.push_back(std::move(item));
    }

    ojson data = ojson::object();
    data["summary"] = summary;
    data["records"] = raw_records;
    return data;
}
