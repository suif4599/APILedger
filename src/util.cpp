#include "util.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace util {

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open file: " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write file: " + path);
    }
    output << content;
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string trim(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::string lower_ascii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string normalize_key(const std::string& value) {
    std::string out = lower_ascii(value);
    out.erase(std::remove(out.begin(), out.end(), ' '), out.end());
    return out;
}

std::string basename(const std::string& path) {
    return fs::path(path).filename().string();
}

std::string stem(const std::string& path) {
    return fs::path(path).stem().string();
}

std::string dirname(const std::string& path) {
    return fs::path(path).parent_path().string();
}

bool make_dirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    return !ec;
}

bool remove_file(const std::string& path) {
    std::error_code ec;
    return fs::remove(path, ec);
}

std::string join_path(const std::string& a, const std::string& b) {
    return (fs::path(a) / b).string();
}

double parse_double(const std::string& value, double fallback) {
    std::string text = value;
    text.erase(std::remove(text.begin(), text.end(), ','), text.end());
    text = trim(text);
    if (text.empty()) return fallback;
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || errno == ERANGE) return fallback;
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0') return fallback;
    return parsed;
}

long long parse_int(const std::string& value, long long fallback) {
    const double parsed = parse_double(value, static_cast<double>(fallback));
    return static_cast<long long>(parsed);
}

std::string format_number(double value, int max_decimals) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(max_decimals);
    out << value;
    return out.str();
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    if (from.empty()) return text;
    size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
    return text;
}

bool ends_with_ci(const std::string& value, const std::string& suffix) {
    if (suffix.size() > value.size()) return false;
    return lower_ascii(value.substr(value.size() - suffix.size())) == lower_ascii(suffix);
}

}  // namespace util
