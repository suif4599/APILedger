#pragma once

#include <optional>
#include <string>
#include <vector>

inline const std::vector<std::string> STANDARD_FIELDS = {
    "bill_start", "platform", "project", "model",
    "type", "tokens", "cost", "unit_price", "currency",
};

struct Record {
    std::string bill_start;
    std::string platform;
    std::string project;
    std::string model;
    std::string type;
    long long tokens = 0;
    double cost = 0.0;
    double unit_price = 0.0;
    std::string source_file;
    std::string currency;
};

struct StoredRecord {
    long long id = 0;
    std::string date;
    std::string platform;
    std::string project;
    std::string model;
    std::string type;
    long long tokens = 0;
    double cost = 0.0;
    double unit_price = 0.0;
    std::string source_file;
    std::string currency;
};

using DateRange = std::pair<std::string, std::string>;

std::optional<DateRange> parse_file_date_range(const std::string& filename);
bool ranges_overlap(const std::optional<DateRange>& a, const std::optional<DateRange>& b);
