#pragma once

#include <string>
#include <vector>

struct Table {
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
};

std::string decode_text(const std::string& bytes);
Table parse_csv_bytes(const std::string& bytes);
Table read_csv(const std::string& path);
