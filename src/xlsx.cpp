#include "xlsx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "miniz/miniz.h"
#include "pugixml/pugixml.hpp"
#include "util.h"

namespace {

using pugi::xml_node;

struct ZipArchive {
    mz_zip_archive archive{};
    std::string data;
    bool open = false;

    ~ZipArchive() {
        if (open) mz_zip_reader_end(&archive);
    }

    bool open_bytes(const std::string& bytes) {
        data = bytes;
        open = mz_zip_reader_init_mem(&archive, data.data(), data.size(), 0) != 0;
        return open;
    }

    std::optional<std::string> read(const std::string& name) {
        if (!open) return std::nullopt;
        size_t size = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&archive, name.c_str(), &size, 0);
        if (!data) return std::nullopt;
        std::string content(static_cast<char*>(data), size);
        mz_free(data);
        return content;
    }
};

std::string xml_text(const xml_node& node) {
    std::string text;
    for (xml_node child : node.children()) {
        if (std::string(child.name()) == "t") {
            text += child.text().get();
        } else {
            text += xml_text(child);
        }
    }
    return text;
}

bool is_builtin_date_format(int id) {
    return (id >= 14 && id <= 22) || (id >= 45 && id <= 47);
}

bool is_date_format_code(std::string code) {
    code = util::lower_ascii(code);
    std::string filtered;
    bool quoted = false;
    bool bracketed = false;
    for (char ch : code) {
        if (ch == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && ch == '[') bracketed = true;
        if (!quoted && ch == ']') bracketed = false;
        if (!quoted && !bracketed) filtered += ch;
    }
    return filtered.find_first_of("ymdhs") != std::string::npos;
}

std::vector<int> read_cell_date_flags(const std::optional<std::string>& styles_xml) {
    std::vector<int> flags;
    if (!styles_xml) return flags;

    pugi::xml_document doc;
    if (!doc.load_buffer(styles_xml->data(), styles_xml->size())) return flags;

    std::vector<std::pair<int, bool>> custom;
    for (const pugi::xpath_node& match : doc.select_nodes("//*[local-name()='numFmt']")) {
        const xml_node node = match.node();
        const int id = node.attribute("numFmtId").as_int(-1);
        const std::string code = node.attribute("formatCode").as_string();
        custom.emplace_back(id, is_date_format_code(code));
    }

    for (const pugi::xpath_node& match :
         doc.select_nodes("//*[local-name()='cellXfs']/*[local-name()='xf']")) {
        const xml_node node = match.node();
        const int id = node.attribute("numFmtId").as_int(0);
        bool is_date = is_builtin_date_format(id);
        for (const auto& entry : custom) {
            if (entry.first == id) {
                is_date = entry.second;
                break;
            }
        }
        flags.push_back(is_date ? 1 : 0);
    }
    return flags;
}

std::string excel_serial_to_text(double serial) {
    long long days = static_cast<long long>(std::floor(serial));
    double fraction = serial - static_cast<double>(days);
    long long seconds = static_cast<long long>(std::llround(fraction * 86400.0));
    if (seconds >= 86400) {
        seconds -= 86400;
        ++days;
    }

    long long z = days - 25569;  // 1899-12-30 -> 1970-01-01
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long year_base = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp + (mp < 10 ? 3 : -9);
    const long long year = year_base + (month <= 2 ? 1 : 0);

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02u", year, month, day);
    std::string text(buffer);
    if (seconds > 0) {
        const long long hour = seconds / 3600;
        const long long minute = (seconds % 3600) / 60;
        const long long second = seconds % 60;
        std::snprintf(buffer, sizeof(buffer), " %02lld:%02lld:%02lld", hour, minute, second);
        text += buffer;
    }
    return text;
}

int column_index_from_ref(const std::string& ref) {
    int index = 0;
    for (char ch : ref) {
        if (ch >= 'A' && ch <= 'Z') {
            index = index * 26 + (ch - 'A' + 1);
        } else if (ch >= 'a' && ch <= 'z') {
            index = index * 26 + (ch - 'a' + 1);
        } else {
            break;
        }
    }
    return index > 0 ? index - 1 : -1;
}

std::string normalize_target(const std::string& target) {
    if (!target.empty() && target[0] == '/') return target.substr(1);
    if (target.rfind("xl/", 0) == 0) return target;
    return "xl/" + target;
}

std::string first_sheet_path(ZipArchive& zip) {
    const auto workbook = zip.read("xl/workbook.xml");
    const auto rels = zip.read("xl/_rels/workbook.xml.rels");
    if (workbook && rels) {
        pugi::xml_document wb_doc;
        pugi::xml_document rel_doc;
        if (wb_doc.load_buffer(workbook->data(), workbook->size()) &&
            rel_doc.load_buffer(rels->data(), rels->size())) {
            for (const pugi::xpath_node& sheet_match :
                 wb_doc.select_nodes("//*[local-name()='sheet']")) {
                const xml_node sheet = sheet_match.node();
                const std::string rid = sheet.attribute("r:id").as_string();
                if (rid.empty()) continue;
                for (const pugi::xpath_node& rel_match :
                     rel_doc.select_nodes("//*[local-name()='Relationship']")) {
                    const xml_node rel = rel_match.node();
                    if (rid == rel.attribute("Id").as_string()) {
                        return normalize_target(rel.attribute("Target").as_string());
                    }
                }
            }
        }
    }

    if (zip.read("xl/worksheets/sheet1.xml")) return "xl/worksheets/sheet1.xml";

    for (unsigned i = 0; i < mz_zip_reader_get_num_files(&zip.archive); ++i) {
        char name[512];
        if (mz_zip_reader_get_filename(&zip.archive, i, name, sizeof(name)) == 0) continue;
        const std::string entry(name);
        if (entry.rfind("xl/worksheets/", 0) == 0 && util::ends_with_ci(entry, ".xml")) {
            return entry;
        }
    }
    return {};
}

}  // namespace

bool is_xlsx_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    char header[4] = {};
    input.read(header, sizeof(header));
    return input.gcount() == 4 && std::string(header, 4) == std::string("PK\x03\x04", 4);
}

Table read_xlsx(const std::string& path) {
    return parse_xlsx_bytes(util::read_file(path));
}

Table parse_xlsx_bytes(const std::string& bytes) {
    Table table;
    ZipArchive zip;
    if (!zip.open_bytes(bytes)) return table;

    const auto shared_xml = zip.read("xl/sharedStrings.xml");
    std::vector<std::string> shared_strings;
    if (shared_xml) {
        pugi::xml_document doc;
        if (doc.load_buffer(shared_xml->data(), shared_xml->size())) {
            for (const pugi::xpath_node& si : doc.select_nodes("//*[local-name()='si']")) {
                shared_strings.push_back(xml_text(si.node()));
            }
        }
    }

    const std::vector<int> date_flags = read_cell_date_flags(zip.read("xl/styles.xml"));

    const std::string sheet_path = first_sheet_path(zip);
    if (sheet_path.empty()) return table;
    const auto sheet_xml = zip.read(sheet_path);
    if (!sheet_xml) return table;

    pugi::xml_document sheet;
    if (!sheet.load_buffer(sheet_xml->data(), sheet_xml->size())) return table;

    std::vector<std::vector<std::string>> grid;
    for (const pugi::xpath_node& row : sheet.select_nodes("//*[local-name()='row']")) {
        xml_node row_node = row.node();
        int row_index = row_node.attribute("r").as_int(static_cast<int>(grid.size()) + 1) - 1;
        if (row_index < 0) row_index = static_cast<int>(grid.size());
        if (static_cast<size_t>(row_index) >= grid.size()) grid.resize(row_index + 1);

        auto& cells = grid[row_index];
        int next_column = 0;
        for (xml_node cell : row_node.children()) {
            if (std::string(cell.name()) != "c") continue;
            int column = column_index_from_ref(cell.attribute("r").as_string());
            if (column < 0) column = next_column;
            next_column = column + 1;
            if (static_cast<size_t>(column) >= cells.size()) cells.resize(column + 1);

            const std::string type = cell.attribute("t").as_string();
            std::string value;
            if (type == "s") {
                const long long index = cell.child("v").text().as_llong(-1);
                if (index >= 0 && static_cast<size_t>(index) < shared_strings.size()) {
                    value = shared_strings[static_cast<size_t>(index)];
                }
            } else if (type == "inlineStr") {
                value = xml_text(cell.child("is"));
            } else if (type == "b") {
                value = cell.child("v").text().as_int() ? "True" : "False";
            } else {
                const xml_node value_node = cell.child("v");
                value = value_node.text().get();
                const int style = cell.attribute("s").as_int(-1);
                if (style >= 0 && static_cast<size_t>(style) < date_flags.size() &&
                    date_flags[static_cast<size_t>(style)] && !value.empty()) {
                    value = excel_serial_to_text(util::parse_double(value));
                }
            }
            cells[column] = value;
        }
    }

    size_t header_index = 0;
    while (header_index < grid.size()) {
        bool empty = true;
        for (const std::string& value : grid[header_index]) {
            if (!value.empty()) {
                empty = false;
                break;
            }
        }
        if (!empty) break;
        ++header_index;
    }
    if (header_index >= grid.size()) return table;

    for (const std::string& header : grid[header_index]) {
        table.headers.push_back(util::trim(header));
    }
    for (size_t r = header_index + 1; r < grid.size(); ++r) {
        std::vector<std::string> data = grid[r];
        data.resize(table.headers.size());
        bool empty = true;
        for (const std::string& value : data) {
            if (!value.empty()) {
                empty = false;
                break;
            }
        }
        if (!empty) table.rows.push_back(std::move(data));
    }
    return table;
}
