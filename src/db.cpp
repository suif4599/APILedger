#include "db.h"

#include <sqlite/sqlite3.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <regex>
#include <set>
#include <stdexcept>

#include "util.h"

namespace fs = std::filesystem;

namespace {

const char* CREATE_TABLE_SQL = R"sql(
CREATE TABLE IF NOT EXISTS api_records (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    date            TEXT NOT NULL,
    platform        TEXT NOT NULL DEFAULT '',
    project         TEXT NOT NULL DEFAULT '',
    model           TEXT NOT NULL DEFAULT '',
    type            TEXT NOT NULL DEFAULT '',
    tokens          INTEGER NOT NULL DEFAULT 0,
    cost            REAL NOT NULL DEFAULT 0.0,
    unit_price      REAL NOT NULL DEFAULT 0.0,
    source_file     TEXT NOT NULL DEFAULT '',
    UNIQUE(date, platform, project, model, type)
)
)sql";

const char* CREATE_INDEXES_SQL[] = {
    "CREATE INDEX IF NOT EXISTS idx_date ON api_records(date);",
    "CREATE INDEX IF NOT EXISTS idx_platform ON api_records(platform);",
    "CREATE INDEX IF NOT EXISTS idx_project ON api_records(project);",
    "CREATE INDEX IF NOT EXISTS idx_model ON api_records(model);",
    "CREATE INDEX IF NOT EXISTS idx_type ON api_records(type);",
};

const char* CREATE_SOURCES_SQL = R"sql(
CREATE TABLE IF NOT EXISTS record_sources (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    date        TEXT NOT NULL,
    platform    TEXT NOT NULL DEFAULT '',
    project     TEXT NOT NULL DEFAULT '',
    model       TEXT NOT NULL DEFAULT '',
    type        TEXT NOT NULL DEFAULT '',
    source_file TEXT NOT NULL DEFAULT '',
    tokens      INTEGER NOT NULL DEFAULT 0,
    cost        REAL NOT NULL DEFAULT 0.0,
    unit_price  REAL NOT NULL DEFAULT 0.0,
    UNIQUE(date, platform, project, model, type, source_file)
)
)sql";

const char* UPSERT_SOURCE_SQL = R"sql(
INSERT INTO record_sources (date, platform, project, model, type, source_file,
                            tokens, cost, unit_price)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(date, platform, project, model, type, source_file)
DO UPDATE SET tokens = excluded.tokens, cost = excluded.cost,
              unit_price = excluded.unit_price
)sql";

const char* UPSERT_AGGREGATE_SQL = R"sql(
INSERT INTO api_records (date, platform, project, model, type,
                         tokens, cost, unit_price, source_file)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(date, platform, project, model, type)
DO UPDATE SET tokens = excluded.tokens, cost = excluded.cost,
              unit_price = excluded.unit_price,
              source_file = excluded.source_file
)sql";

struct Statement {
    sqlite3_stmt* handle = nullptr;

    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &handle, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("sqlite prepare failed: ") + sqlite3_errmsg(db));
        }
    }

    ~Statement() {
        if (handle) sqlite3_finalize(handle);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void reset() {
        sqlite3_reset(handle);
        sqlite3_clear_bindings(handle);
    }

    bool step() {
        const int code = sqlite3_step(handle);
        if (code == SQLITE_ROW) return true;
        if (code == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("sqlite step failed: ") + sqlite3_errstr(code));
    }
};

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
}

std::string column_text(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    if (!text) return {};
    return std::string(reinterpret_cast<const char*>(text));
}

std::string key_date(const ojson& row) {
    const std::string value = json_text(row.value("bill_start", ojson("")));
    return value.size() > 10 ? value.substr(0, 10) : value;
}

std::vector<std::string> normalize_files(const std::vector<std::string>& files) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const std::string& file : files) {
        if (file.empty() || seen.count(file)) continue;
        seen.insert(file);
        out.push_back(file);
    }
    return out;
}

void bind_key(sqlite3_stmt* stmt, const std::vector<std::string>& key, int start = 1) {
    for (size_t i = 0; i < key.size(); ++i) {
        bind_text(stmt, start + static_cast<int>(i), key[i]);
    }
}

}  // namespace

std::string json_text(const ojson& value) {
    if (value.is_null()) return {};
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "True" : "False";
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float()) return value.dump();
    return value.dump();
}

long long json_int(const ojson& value) {
    if (value.is_null()) return 0;
    if (value.is_number_integer()) return value.get<long long>();
    if (value.is_number_unsigned()) return static_cast<long long>(value.get<unsigned long long>());
    if (value.is_number_float()) return static_cast<long long>(value.get<double>());
    if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
    if (value.is_string()) return util::parse_int(value.get<std::string>(), 0);
    return 0;
}

double json_number(const ojson& value) {
    if (value.is_null()) return 0.0;
    if (value.is_number()) return value.get<double>();
    if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
    if (value.is_string()) return util::parse_double(value.get<std::string>(), 0.0);
    return 0.0;
}

std::vector<std::string> record_key(const ojson& row) {
    return {
        key_date(row),
        json_text(row.value("platform", ojson(""))),
        json_text(row.value("project", ojson(""))),
        json_text(row.value("model", ojson(""))),
        json_text(row.value("type", ojson(""))),
    };
}

std::optional<DateRange> parse_file_date_range(const std::string& filename) {
    static const std::regex date_re(R"((\d{4}-\d{2}-\d{2}))");
    const std::string base = util::basename(filename);
    std::vector<std::string> dates;
    for (auto it = std::sregex_iterator(base.begin(), base.end(), date_re);
         it != std::sregex_iterator(); ++it) {
        dates.push_back((*it)[1].str());
    }
    if (dates.empty()) return std::nullopt;
    auto [minimum, maximum] = std::minmax_element(dates.begin(), dates.end());
    return DateRange{*minimum, *maximum};
}

bool ranges_overlap(const std::optional<DateRange>& a, const std::optional<DateRange>& b) {
    if (!a || !b) return true;
    return a->first <= b->second && b->first <= a->second;
}

Database::Database(const std::string& path) : path_(path) {}

Database::~Database() { close(); }

void Database::connect() {
    const std::string parent = util::dirname(path_);
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error(std::string("cannot open database: ") + sqlite3_errmsg(db_));
    }
    sqlite3_exec(db_, "PRAGMA journal_mode=DELETE;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
    init_tables();
}

void Database::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::table_exists(const std::string& name) {
    Statement stmt(db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
    bind_text(stmt.handle, 1, name);
    return stmt.step();
}

void Database::init_tables() {
    if (table_exists("api_records")) {
        Statement stmt(db_, "PRAGMA table_info(api_records)");
        std::vector<std::string> columns;
        while (stmt.step()) {
            columns.push_back(column_text(stmt.handle, 1));
        }
        if (std::find(columns.begin(), columns.end(), "date") == columns.end()) {
            std::cout << "  [database] legacy table schema detected, rebuilding with the date column\n";
            sqlite3_exec(db_, "DROP TABLE IF EXISTS api_records", nullptr, nullptr, nullptr);
        }
    }

    if (sqlite3_exec(db_, CREATE_TABLE_SQL, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("cannot create table: ") + sqlite3_errmsg(db_));
    }
    for (const char* sql : CREATE_INDEXES_SQL) {
        sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
    }
    if (sqlite3_exec(db_, CREATE_SOURCES_SQL, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("cannot create sources table: ") +
                                 sqlite3_errmsg(db_));
    }

    long long source_count = 0;
    long long record_count = 0;
    {
        Statement stmt(db_, "SELECT COUNT(*) FROM record_sources");
        if (stmt.step()) source_count = sqlite3_column_int64(stmt.handle, 0);
    }
    {
        Statement stmt(db_, "SELECT COUNT(*) FROM api_records");
        if (stmt.step()) record_count = sqlite3_column_int64(stmt.handle, 0);
    }
    if (record_count > 0 && source_count == 0) {
        std::cout << "  [database] migrating: backfilling record_sources from api_records\n";
        sqlite3_exec(db_,
                     "INSERT INTO record_sources (date, platform, project, model, type,"
                     " source_file, tokens, cost, unit_price) "
                     "SELECT date, platform, project, model, type, source_file,"
                     " tokens, cost, unit_price FROM api_records",
                     nullptr, nullptr, nullptr);
    }

    for (const char* table : {"api_records", "record_sources"}) {
        const std::string sql =
            std::string("UPDATE ") + table +
            " SET type = CASE type"
            " WHEN '缓存输入' THEN 'cache_input'"
            " WHEN '输入' THEN 'input'"
            " WHEN '输出' THEN 'output' ELSE type END"
            " WHERE type IN ('缓存输入','输入','输出')";
        sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    }
}

void Database::refresh_aggregate(const std::vector<std::string>& key) {
    Statement query(db_,
                    "SELECT COUNT(*), SUM(tokens), SUM(cost), MAX(unit_price),"
                    " GROUP_CONCAT(DISTINCT source_file) FROM record_sources"
                    " WHERE date=? AND platform=? AND project=? AND model=? AND type=?");
    bind_key(query.handle, key);
    if (!query.step()) return;

    const long long count = sqlite3_column_int64(query.handle, 0);
    if (count == 0) {
        Statement remove(db_,
                         "DELETE FROM api_records WHERE date=? AND platform=? AND"
                         " project=? AND model=? AND type=?");
        bind_key(remove.handle, key);
        remove.step();
        return;
    }

    const long long tokens = sqlite3_column_int64(query.handle, 1);
    const double cost = sqlite3_column_double(query.handle, 2);
    const double max_price = sqlite3_column_double(query.handle, 3);
    const std::string files = column_text(query.handle, 4);
    const double unit_price =
        tokens > 0 ? std::round(cost * 1e6 / static_cast<double>(tokens) * 1e4) / 1e4
                   : max_price;

    Statement upsert(db_, UPSERT_AGGREGATE_SQL);
    bind_key(upsert.handle, key);
    sqlite3_bind_int64(upsert.handle, 6, tokens);
    sqlite3_bind_double(upsert.handle, 7, cost);
    sqlite3_bind_double(upsert.handle, 8, unit_price);
    bind_text(upsert.handle, 9, files);
    upsert.step();
}

int Database::upsert_batch(const std::vector<ojson>& records) {
    if (records.empty()) return 0;

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    try {
        Statement source(db_, UPSERT_SOURCE_SQL);
        for (const ojson& record : records) {
            const std::vector<std::string> key = record_key(record);
            source.reset();
            bind_key(source.handle, key);
            bind_text(source.handle, 6, json_text(record.value("source_file", ojson(""))));
            sqlite3_bind_int64(source.handle, 7, json_int(record.value("tokens", ojson(0))));
            sqlite3_bind_double(source.handle, 8, json_number(record.value("cost", ojson(0))));
            sqlite3_bind_double(source.handle, 9,
                                json_number(record.value("unit_price", ojson(0))));
            source.step();
            refresh_aggregate(key);
        }
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    return static_cast<int>(records.size());
}

std::vector<std::string> Database::find_overlapping_source_files(
    const std::vector<std::string>& platforms,
    const std::optional<DateRange>& new_range) {
    const std::vector<std::string> names = normalize_files(platforms);

    std::string sql =
        "SELECT DISTINCT source_file FROM record_sources WHERE source_file != ''";
    if (!names.empty()) {
        sql += " AND platform IN (";
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) sql += ",";
            sql += "?";
        }
        sql += ")";
    }
    sql += " ORDER BY source_file";

    Statement stmt(db_, sql.c_str());
    for (size_t i = 0; i < names.size(); ++i) {
        bind_text(stmt.handle, static_cast<int>(i) + 1, names[i]);
    }

    std::vector<std::string> result;
    while (stmt.step()) {
        const std::string file = column_text(stmt.handle, 0);
        if (ranges_overlap(new_range, parse_file_date_range(file))) {
            result.push_back(file);
        }
    }
    return result;
}

int Database::delete_sources(const std::vector<std::string>& source_files) {
    const std::vector<std::string> files = normalize_files(source_files);
    if (files.empty()) return 0;

    std::string placeholders;
    for (size_t i = 0; i < files.size(); ++i) {
        if (i) placeholders += ",";
        placeholders += "?";
    }

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    try {
        std::vector<std::vector<std::string>> keys;
        {
            Statement select(db_,
                             ("SELECT DISTINCT date, platform, project, model, type"
                              " FROM record_sources WHERE source_file IN (" +
                              placeholders + ")")
                                 .c_str());
            for (size_t i = 0; i < files.size(); ++i) {
                bind_text(select.handle, static_cast<int>(i) + 1, files[i]);
            }
            while (select.step()) {
                keys.push_back({column_text(select.handle, 0), column_text(select.handle, 1),
                                column_text(select.handle, 2), column_text(select.handle, 3),
                                column_text(select.handle, 4)});
            }
        }

        int deleted = 0;
        {
            Statement remove(db_,
                             ("DELETE FROM record_sources WHERE source_file IN (" +
                              placeholders + ")")
                                 .c_str());
            for (size_t i = 0; i < files.size(); ++i) {
                bind_text(remove.handle, static_cast<int>(i) + 1, files[i]);
            }
            remove.step();
            deleted = sqlite3_changes(db_);
        }

        for (const auto& key : keys) refresh_aggregate(key);

        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
        return deleted;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

std::vector<StoredRecord> Database::get_all(const std::string& order_by) {
    Statement query(db_, ("SELECT * FROM api_records ORDER BY " + order_by).c_str());
    std::vector<StoredRecord> records;
    while (query.step()) {
        StoredRecord record;
        record.id = sqlite3_column_int64(query.handle, 0);
        record.date = column_text(query.handle, 1);
        record.platform = column_text(query.handle, 2);
        record.project = column_text(query.handle, 3);
        record.model = column_text(query.handle, 4);
        record.type = column_text(query.handle, 5);
        record.tokens = sqlite3_column_int64(query.handle, 6);
        record.cost = sqlite3_column_double(query.handle, 7);
        record.unit_price = sqlite3_column_double(query.handle, 8);
        record.source_file = column_text(query.handle, 9);
        records.push_back(std::move(record));
    }
    return records;
}
