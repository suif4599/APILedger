#include "csv.h"

#include <iconv.h>

#include <cerrno>
#include <optional>

#include "util.h"

namespace {

bool valid_utf8(const std::string& text) {
    size_t i = 0;
    const size_t n = text.size();
    static const unsigned min_codepoint[] = {0, 0x80, 0x800, 0x10000};

    while (i < n) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }

        int extra = 0;
        unsigned codepoint = 0;
        if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            codepoint = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
            codepoint = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
            codepoint = lead & 0x07;
        } else {
            return false;
        }

        if (i + extra >= n) return false;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char ch = static_cast<unsigned char>(text[i + k]);
            if ((ch & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (ch & 0x3F);
        }
        if (codepoint < min_codepoint[extra]) return false;
        if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
        i += static_cast<size_t>(extra) + 1;
    }
    return true;
}

std::optional<std::string> iconv_to_utf8(const std::string& input, const char* from) {
    iconv_t handle = iconv_open("UTF-8", from);
    if (handle == reinterpret_cast<iconv_t>(-1)) return std::nullopt;

    std::string output;
    output.resize(input.size() * 2 + 16);
    char* in_ptr = const_cast<char*>(input.data());
    size_t in_left = input.size();
    char* out_ptr = output.data();
    size_t out_left = output.size();

    while (in_left > 0) {
        const size_t result = iconv(handle, &in_ptr, &in_left, &out_ptr, &out_left);
        if (result == static_cast<size_t>(-1)) {
            if (errno == E2BIG) {
                const size_t used = static_cast<size_t>(out_ptr - output.data());
                output.resize(output.size() * 2 + 16);
                out_ptr = output.data() + used;
                out_left = output.size() - used;
                continue;
            }
            iconv_close(handle);
            return std::nullopt;
        }
    }

    const size_t used = static_cast<size_t>(out_ptr - output.data());
    iconv_close(handle);
    output.resize(used);
    return output;
}

bool row_is_empty(const std::vector<std::string>& row) {
    for (const std::string& field : row) {
        if (!field.empty()) return false;
    }
    return true;
}

}  // namespace

std::string decode_text(const std::string& bytes) {
    std::string text = bytes;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    if (valid_utf8(text)) return text;

    if (auto converted = iconv_to_utf8(text, "GB18030")) return *converted;
    if (auto converted = iconv_to_utf8(text, "ISO-8859-1")) return *converted;
    return text;
}

Table parse_csv_bytes(const std::string& bytes) {
    const std::string text = decode_text(bytes);

    std::vector<std::vector<std::string>> raw_rows;
    std::vector<std::string> row;
    std::string field;
    bool in_quotes = false;

    size_t i = 0;
    while (i < text.size()) {
        const char ch = text[i];
        if (in_quotes) {
            if (ch == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    i += 2;
                    continue;
                }
                in_quotes = false;
                ++i;
                continue;
            }
            field += ch;
            ++i;
            continue;
        }

        if (ch == '"') {
            in_quotes = true;
            ++i;
        } else if (ch == ',') {
            row.push_back(field);
            field.clear();
            ++i;
        } else if (ch == '\r' || ch == '\n') {
            row.push_back(field);
            field.clear();
            raw_rows.push_back(row);
            row.clear();
            ++i;
            if (ch == '\r' && i < text.size() && text[i] == '\n') ++i;
        } else {
            field += ch;
            ++i;
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(field);
        raw_rows.push_back(row);
    }

    Table table;
    size_t header_index = 0;
    while (header_index < raw_rows.size() && row_is_empty(raw_rows[header_index])) {
        ++header_index;
    }
    if (header_index >= raw_rows.size()) return table;

    for (const std::string& header : raw_rows[header_index]) {
        table.headers.push_back(util::trim(header));
    }

    for (size_t r = header_index + 1; r < raw_rows.size(); ++r) {
        if (row_is_empty(raw_rows[r])) continue;
        std::vector<std::string> data = raw_rows[r];
        data.resize(table.headers.size());
        table.rows.push_back(std::move(data));
    }
    return table;
}

Table read_csv(const std::string& path) {
    return parse_csv_bytes(util::read_file(path));
}
