#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "json.h"
#include "models.h"

struct sqlite3;

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void connect();
    void close();

    int upsert_batch(const std::vector<ojson>& records);
    std::vector<std::string> find_overlapping_source_files(
        const std::vector<std::string>& platforms,
        const std::optional<DateRange>& new_range);
    int delete_sources(const std::vector<std::string>& source_files);
    std::vector<StoredRecord> get_all(const std::string& order_by = "date DESC");

private:
    void init_tables();
    bool table_exists(const std::string& name);
    void refresh_aggregate(const std::vector<std::string>& key);

    std::string path_;
    sqlite3* db_ = nullptr;
};

std::vector<std::string> record_key(const ojson& row);
std::string json_text(const ojson& value);
long long json_int(const ojson& value);
double json_number(const ojson& value);
