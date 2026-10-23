#include "importer.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <regex>
#include <set>

#include "csv.h"
#include "expr.h"
#include "presets.h"
#include "util.h"
#include "xlsx.h"
#include "zip.h"

namespace {

struct RowRules {
    ojson defaults = ojson::object();
    ojson type_map = ojson::object();
    ojson computed = ojson::object();
    ojson model_map = ojson::object();
    std::vector<std::string> skip_types;
    std::vector<std::pair<std::string, std::vector<std::string>>> skip_if;

    bool has_extract = false;
    std::string extract_column;
    std::regex extract_regex;
    int group_type = -1;
    int group_tokens = -1;

    bool has_date_format = false;
    std::string date_field;
    std::string date_format;
};

std::vector<std::string> as_string_list(const ojson& value) {
    std::vector<std::string> out;
    if (!value.is_array()) return out;
    for (const auto& item : value) {
        out.push_back(json_text(item));
    }
    return out;
}

RowRules build_rules(const ojson& config) {
    RowRules rules;
    rules.defaults = config.value("defaults", ojson::object());
    rules.type_map = config.value("type_map", ojson::object());
    rules.computed = config.value("computed", ojson::object());
    rules.model_map = config.value("model_map", ojson::object());
    rules.skip_types = as_string_list(config.value("skip_types", ojson::array()));

    if (!config.contains("parse") || !config["parse"].is_object()) return rules;
    const ojson& parse = config["parse"];

    if (parse.contains("skip_if") && parse["skip_if"].is_object()) {
        for (auto it = parse["skip_if"].begin(); it != parse["skip_if"].end(); ++it) {
            rules.skip_if.emplace_back(it.key(), as_string_list(it.value()));
        }
    }

    if (parse.contains("extract") && parse["extract"].is_object()) {
        const ojson& extract = parse["extract"];
        rules.extract_column = extract.value("column", std::string());
        rules.group_type = extract.value("group_type", -1);
        rules.group_tokens = extract.value("group_tokens", -1);
        const std::string pattern = extract.value("pattern", std::string());
        if (!rules.extract_column.empty() && !pattern.empty()) {
            try {
                rules.extract_regex = std::regex(pattern);
                rules.has_extract = true;
            } catch (const std::regex_error& error) {
                std::cout << "  [preset] invalid regex (" << pattern << "): " << error.what() << "\n";
            }
        }
    }

    if (parse.contains("date_format") && parse["date_format"].is_object()) {
        const ojson& date_format = parse["date_format"];
        rules.date_field = date_format.value("field", std::string());
        rules.date_format = date_format.value("from", std::string());
        rules.has_date_format = !rules.date_field.empty() && rules.date_format == "yyyyMMdd";
    }
    return rules;
}

std::string field_string(const ojson& entry, const std::string& key) {
    if (!entry.contains(key)) return {};
    return json_text(entry[key]);
}

std::string raw_value(const std::map<std::string, std::string>& raw, const std::string& key) {
    const auto it = raw.find(key);
    return it == raw.end() ? std::string() : it->second;
}

std::string current_value(const ojson& entry, const std::map<std::string, std::string>& raw,
                          const std::string& field) {
    if (entry.contains(field)) return field_string(entry, field);
    return raw_value(raw, field);
}

bool in_string_list(const std::string& value, const std::vector<std::string>& values) {
    const std::string trimmed = util::trim(value);
    return std::find(values.begin(), values.end(), trimmed) != values.end();
}

std::optional<ojson> apply_preset_row(const RowRules& rules,
                                      const std::map<std::string, std::string>& raw,
                                      ojson entry) {
    if (rules.defaults.is_object()) {
        for (auto it = rules.defaults.begin(); it != rules.defaults.end(); ++it) {
            bool empty = !entry.contains(it.key()) || entry[it.key()].is_null() ||
                         (entry[it.key()].is_string() &&
                          entry[it.key()].get<std::string>().empty());
            if (empty) entry[it.key()] = it.value();
        }
    }

    for (const auto& [field, values] : rules.skip_if) {
        if (in_string_list(current_value(entry, raw, field), values)) {
            return std::nullopt;
        }
    }

    if (rules.has_extract) {
        std::smatch match;
        const std::string source = raw_value(raw, rules.extract_column);
        if (!std::regex_match(source, match, rules.extract_regex)) {
            return std::nullopt;
        }
        std::string raw_type;
        if (rules.group_type > 0 && rules.group_type < static_cast<int>(match.size())) {
            raw_type = match[rules.group_type].str();
            if (in_string_list(raw_type, rules.skip_types)) return std::nullopt;
            if (rules.type_map.is_object() && rules.type_map.contains(raw_type)) {
                entry["type"] = rules.type_map[raw_type];
            } else {
                entry["type"] = raw_type;
            }
        }
        if (rules.group_tokens > 0 &&
            rules.group_tokens < static_cast<int>(match.size())) {
            entry["tokens"] = util::parse_int(match[rules.group_tokens].str(), 0);
        }
    }

    if (rules.has_date_format) {
        const std::string value = field_string(entry, rules.date_field);
        static const std::regex compact_date(R"(^(\d{4})(\d{2})(\d{2})$)");
        std::smatch match;
        if (std::regex_match(value, match, compact_date)) {
            entry[rules.date_field] =
                match[1].str() + "-" + match[2].str() + "-" + match[3].str();
        }
    }

    if (rules.type_map.is_object()) {
        const std::string raw_type = field_string(entry, "type");
        if (rules.type_map.contains(raw_type)) {
            if (rules.type_map[raw_type].is_string() &&
                !rules.type_map[raw_type].get<std::string>().empty()) {
                entry["type"] = rules.type_map[raw_type];
            }
        }
    }

    if (rules.computed.is_object()) {
        for (auto it = rules.computed.begin(); it != rules.computed.end(); ++it) {
            if (!it.value().is_string()) continue;
            const std::string expression = it.value().get<std::string>();
            const auto value = compute_field(
                expression, [&](const std::string& name) {
                    return entry.contains(name) ? json_number(entry[name]) : 0.0;
                });
            if (value) entry[it.key()] = *value;
        }
    }

    if (rules.model_map.is_object()) {
        const std::string model = field_string(entry, "model");
        if (!model.empty() && rules.model_map.contains(model)) {
            entry["model"] = rules.model_map[model];
        }
    }

    return entry;
}

double round4(double value) { return std::round(value * 1e4) / 1e4; }

std::vector<ojson> merge_by_key(const std::vector<ojson>& records) {
    std::vector<ojson> merged;
    std::map<std::vector<std::string>, size_t> index;

    for (const ojson& record : records) {
        const std::vector<std::string> key = record_key(record);
        const auto found = index.find(key);
        if (found == index.end()) {
            index[key] = merged.size();
            merged.push_back(record);
            continue;
        }

        ojson& target = merged[found->second];
        target["tokens"] = json_int(target.value("tokens", ojson(0))) +
                           json_int(record.value("tokens", ojson(0)));
        target["cost"] = json_number(target.value("cost", ojson(0))) +
                         json_number(record.value("cost", ojson(0)));

        const std::string new_source = field_string(record, "source_file");
        const std::string current_source = field_string(target, "source_file");
        if (!new_source.empty() &&
            current_source.find(new_source) == std::string::npos) {
            std::string combined = current_source + "," + new_source;
            const size_t begin = combined.find_first_not_of(',');
            const size_t end = combined.find_last_not_of(',');
            combined = begin == std::string::npos ? std::string()
                                                   : combined.substr(begin, end - begin + 1);
            target["source_file"] = combined;
        }
    }
    return merged;
}

void post_process(std::vector<ojson>& records) {
    for (ojson& entry : records) {
        if (json_number(entry.value("unit_price", ojson(0))) == 0.0) {
            const long long tokens = json_int(entry.value("tokens", ojson(0)));
            const double cost = json_number(entry.value("cost", ojson(0)));
            if (tokens > 0 && cost > 0) {
                entry["unit_price"] = round4(cost * 1e6 / static_cast<double>(tokens));
            }
        }
    }

    const size_t before = records.size();
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [](const ojson& entry) {
                                     const long long tokens =
                                         json_int(entry.value("tokens", ojson(0)));
                                     const double cost =
                                         json_number(entry.value("cost", ojson(0)));
                                     return tokens == 0 && std::abs(cost) <= 1e-9;
                                 }),
                  records.end());
    if (records.size() != before) {
        std::cout << "     filtered: " << (before - records.size())
                  << " rows (no tokens and no cost)\n";
    }
}

std::vector<ojson> parse_table_records(const Table& table, const std::string& filename,
                                       const std::vector<Preset>& presets) {
    const Preset* preset = detect_preset(filename, table.headers, presets);
    if (!preset) return {};

    const ojson& config = preset->config;
    const auto column_mapping = apply_column_mapping(config["column_mapping"], table.headers);
    const RowRules rules = build_rules(config);

    std::cout << "\n  [read] " << filename << "\n";
    std::cout << "     preset: " << preset_name(*preset) << "\n";
    std::cout << "     headers: [";
    for (size_t i = 0; i < table.headers.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << "'" << table.headers[i] << "'";
    }
    std::cout << "]\n";

    ojson mapping = ojson::object();
    std::set<std::string> mapped_headers;
    for (const auto& [field, header] : column_mapping) {
        mapping[field] = header;
        mapped_headers.insert(header);
    }
    std::cout << "     fields: " << mapping.dump() << "\n";

    std::cout << "     ignored columns: [";
    bool first_ignored = true;
    for (const std::string& header : table.headers) {
        if (mapped_headers.count(header)) continue;
        if (!first_ignored) std::cout << ", ";
        std::cout << "'" << header << "'";
        first_ignored = false;
    }
    std::cout << "]\n";
    std::cout << "     rows: " << table.rows.size() << "\n";

    std::vector<ojson> parsed;
    for (const auto& values : table.rows) {
        std::map<std::string, std::string> raw;
        for (size_t i = 0; i < table.headers.size() && i < values.size(); ++i) {
            raw[table.headers[i]] = values[i];
        }

        ojson entry = ojson::object();
        for (const std::string& field : STANDARD_FIELDS) {
            const auto it = column_mapping.find(field);
            if (it == column_mapping.end()) {
                if (field == "tokens") {
                    entry[field] = 0;
                } else if (field == "cost" || field == "unit_price") {
                    entry[field] = 0.0;
                } else {
                    entry[field] = "";
                }
                continue;
            }

            const std::string value = raw_value(raw, it->second);
            if (field == "unit_price") {
                entry[field] = util::parse_double(value, 0.0);
            } else if (field == "tokens") {
                entry[field] = util::parse_int(value, 0);
            } else if (field == "cost") {
                entry[field] = util::parse_double(value, 0.0);
            } else {
                entry[field] = value;
            }
        }

        auto result = apply_preset_row(rules, raw, std::move(entry));
        if (!result) continue;

        const std::string bill_start = field_string(*result, "bill_start");
        if (!bill_start.empty()) {
            (*result)["bill_start"] = bill_start.substr(0, std::min<size_t>(10, bill_start.size()));
        }
        (*result)["source_file"] = filename;
        parsed.push_back(std::move(*result));
    }

    post_process(parsed);

    const size_t before = parsed.size();
    parsed = merge_by_key(parsed);
    if (parsed.size() != before) {
        std::cout << "     merged: " << (before - parsed.size())
                  << " rows with the same key\n";
    }

    std::set<std::string> models;
    std::set<std::string> types;
    std::set<std::string> platforms;
    long long total_tokens = 0;
    double total_cost = 0.0;
    for (const ojson& record : parsed) {
        const std::string model = field_string(record, "model");
        const std::string type = field_string(record, "type");
        const std::string platform = field_string(record, "platform");
        if (!model.empty()) models.insert(model);
        if (!type.empty()) types.insert(type);
        if (!platform.empty()) platforms.insert(platform);
        total_tokens += json_int(record.value("tokens", ojson(0)));
        total_cost += json_number(record.value("cost", ojson(0)));
    }
    std::cout << "     done: " << parsed.size() << " rows -> " << platforms.size()
              << " platforms, " << models.size() << " models, " << types.size() << " types\n";
    std::cout << "     tokens: " << total_tokens << ", cost: "
              << util::format_number(total_cost, 2) << "\n";

    return parsed;
}

bool has_zip_magic(const std::string& bytes) {
    return bytes.size() >= 4 && bytes.compare(0, 4, std::string("PK\x03\x04", 4)) == 0;
}

}  // namespace

namespace {

std::string detect_currency(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::set<std::string> currencies;
    for (const auto& [name, bytes] : entries) {
        if (!util::ends_with_ci(name, ".csv")) continue;
        try {
            const Table table = parse_csv_bytes(bytes);
            int column = -1;
            for (size_t i = 0; i < table.headers.size(); ++i) {
                if (util::lower_ascii(util::trim(table.headers[i])) == "currency") {
                    column = static_cast<int>(i);
                    break;
                }
            }
            if (column < 0) continue;
            for (const auto& row : table.rows) {
                if (static_cast<size_t>(column) >= row.size()) continue;
                const std::string value = util::trim(row[column]);
                if (!value.empty()) currencies.insert(value);
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    if (currencies.size() > 1) {
        std::cout << "  [currency] multiple currencies in archive; using preset default\n";
    }
    return currencies.size() == 1 ? *currencies.begin() : std::string();
}

bool has_currency_column(const std::string& bytes) {
    if (has_zip_magic(bytes)) return false;
    try {
        const Table table = parse_csv_bytes(bytes);
        for (const std::string& header : table.headers) {
            if (util::lower_ascii(util::trim(header)) == "currency") return true;
        }
    } catch (const std::exception&) {
        return false;
    }
    return false;
}

}  // namespace

SourceBundle load_sources_bytes(const std::string& name, const std::string& bytes) {
    SourceBundle bundle;
    const std::vector<Preset> presets = load_all_presets();
    const std::optional<DateRange> fallback_range = parse_file_date_range(name);

    std::vector<std::pair<std::string, std::string>> entries;
    if (util::ends_with_ci(name, ".zip")) {
        for (const ZipEntry& entry : read_zip_entries_from_mem(bytes)) {
            if (util::ends_with_ci(entry.name, ".csv") ||
                util::ends_with_ci(entry.name, ".xlsx")) {
                entries.emplace_back(entry.name, entry.bytes);
            }
        }
    } else {
        entries.emplace_back(util::basename(name), bytes);
    }

    const std::string archive_currency = detect_currency(entries);
    if (!archive_currency.empty()) {
        std::cout << "  [currency] " << archive_currency << "\n";
    }

    auto add_source = [&](const std::string& raw_name, const std::string& source_bytes) {
        const std::string source_name = util::basename(raw_name);
        try {
            const Table table = has_zip_magic(source_bytes) ? parse_xlsx_bytes(source_bytes)
                                                            : parse_csv_bytes(source_bytes);
            std::vector<ojson> records = parse_table_records(table, source_name, presets);
            if (records.empty()) {
                if (!has_currency_column(source_bytes)) {
                    bundle.skipped.push_back({source_name, "no matching preset or no data"});
                }
                return;
            }
            if (!archive_currency.empty()) {
                for (ojson& record : records) record["currency"] = archive_currency;
            }
            auto range = parse_file_date_range(source_name);
            if (!range) range = fallback_range;
            bundle.files.push_back({source_name, range, std::move(records)});
        } catch (const std::exception& error) {
            bundle.skipped.push_back({source_name, error.what()});
        }
    };

    for (const auto& [entry_name, entry_bytes] : entries) {
        add_source(entry_name, entry_bytes);
    }
    return bundle;
}

SourceBundle load_sources(const std::string& path) {
    return load_sources_bytes(util::basename(path), util::read_file(path));
}

ReplaceResult import_parsed_file(Database& db, const ParsedFile& file) {
    ReplaceResult result;
    result.name = file.name;
    result.date_range = file.date_range;
    result.records = static_cast<int>(file.records.size());
    if (file.records.empty()) return result;

    std::set<std::string> platform_set;
    for (const ojson& record : file.records) {
        const std::string platform = field_string(record, "platform");
        if (!platform.empty()) platform_set.insert(platform);
    }
    result.platforms.assign(platform_set.begin(), platform_set.end());

    result.replaced = db.find_overlapping_source_files(result.platforms, result.date_range);

    std::string platforms_text;
    for (size_t i = 0; i < result.platforms.size(); ++i) {
        if (i) platforms_text += ", ";
        platforms_text += result.platforms[i];
    }
    if (platforms_text.empty()) platforms_text = "(unspecified)";

    const std::string range_text =
        result.date_range ? result.date_range->first + " ~ " + result.date_range->second
                          : "unknown (no date in filename)";
    std::cout << "     range: platforms " << platforms_text << " | dates " << range_text << "\n";

    if (!result.replaced.empty()) {
        std::cout << "     merging " << result.replaced.size() << " overlapping bill(s): ";
        for (size_t i = 0; i < result.replaced.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << result.replaced[i];
        }
        std::cout << "\n";
    } else {
        std::cout << "     no overlapping bills, accumulating\n";
    }

    const Database::MergeResult merged = db.merge_import(file.records, result.replaced);
    result.deleted = merged.deleted_files;
    result.written = merged.written;
    result.dropped = merged.dropped;
    if (merged.dropped > 0) {
        std::cout << "     kept " << merged.dropped
                  << " existing row(s) with equal or higher values\n";
    }
    return result;
}
