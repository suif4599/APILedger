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
    void connect_memory();
    void close();

    bool load_snapshot(const std::string& bytes);
    std::string snapshot() const;
    std::string export_dirty();
    void apply_state(const std::string& json);

    struct MergeResult {
        int written = 0;
        int dropped = 0;
        int deleted_files = 0;
    };

    struct DeleteResult {
        int rows = 0;
    };

    MergeResult merge_import(const std::vector<ojson>& records,
                             const std::vector<std::string>& old_files);
    DeleteResult delete_date_range(const std::string& start, const std::string& end);
    std::vector<std::string> find_overlapping_source_files(
        const std::vector<std::string>& platforms,
        const std::optional<DateRange>& new_range);
    std::vector<StoredRecord> get_all(const std::string& order_by = "date DESC");

private:
    void init_tables();
    bool table_exists(const std::string& name);
    void refresh_aggregate(const std::vector<std::string>& key);
    void rebuild_aggregates();
    void mark_source(const std::string& source_file, bool deleted);

    std::string path_;
    sqlite3* db_ = nullptr;
};

std::vector<std::string> record_key(const ojson& row);
std::string json_text(const ojson& value);
long long json_int(const ojson& value);
double json_number(const ojson& value);
