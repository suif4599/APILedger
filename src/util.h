#pragma once

#include <optional>
#include <string>

namespace util {

std::string read_file(const std::string& path);
void write_file(const std::string& path, const std::string& content);
bool file_exists(const std::string& path);
std::string trim(const std::string& value);
std::string lower_ascii(std::string value);
std::string normalize_key(const std::string& value);
std::string basename(const std::string& path);
std::string stem(const std::string& path);
std::string dirname(const std::string& path);
bool make_dirs(const std::string& path);
bool remove_file(const std::string& path);
std::string join_path(const std::string& a, const std::string& b);

double parse_double(const std::string& value, double fallback = 0.0);
long long parse_int(const std::string& value, long long fallback = 0);
std::string format_number(double value, int max_decimals);
std::string replace_all(std::string text, const std::string& from, const std::string& to);
bool ends_with_ci(const std::string& value, const std::string& suffix);

}
