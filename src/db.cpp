#include "db.h"

#include <sqlite/sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <tuple>

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
    currency        TEXT NOT NULL DEFAULT '',
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
    currency    TEXT NOT NULL DEFAULT '',
    UNIQUE(date, platform, project, model, type, source_file)
)
)sql";

const char* CREATE_STATE_SQL = R"sql(
CREATE TABLE IF NOT EXISTS source_state (
    source_file TEXT PRIMARY KEY,
    updated_at  INTEGER NOT NULL DEFAULT 0,
    deleted     INTEGER NOT NULL DEFAULT 0,
    dirty       INTEGER NOT NULL DEFAULT 0
)
)sql";

const char* MARK_SOURCE_SQL = R"sql(
INSERT INTO source_state (source_file, updated_at, deleted, dirty)
VALUES (?, ?, ?, 1)
ON CONFLICT(source_file) DO UPDATE SET
    updated_at = MAX(excluded.updated_at, source_state.updated_at + 1),
    deleted = excluded.deleted,
    dirty = 1
)sql";

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

const char* UPSERT_SOURCE_SQL = R"sql(
INSERT INTO record_sources (date, platform, project, model, type, source_file,
                            tokens, cost, unit_price, currency)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(date, platform, project, model, type, source_file)
DO UPDATE SET tokens = excluded.tokens, cost = excluded.cost,
              unit_price = excluded.unit_price, currency = excluded.currency
)sql";

const char* UPSERT_AGGREGATE_SQL = R"sql(
INSERT INTO api_records (date, platform, project, model, type,
                         tokens, cost, unit_price, source_file, currency)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(date, platform, project, model, type)
DO UPDATE SET tokens = excluded.tokens, cost = excluded.cost,
              unit_price = excluded.unit_price,
              source_file = excluded.source_file,
              currency = excluded.currency
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

void Database::connect_memory() {
    if (sqlite3_open(":memory:", &db_) != SQLITE_OK) {
        throw std::runtime_error(std::string("cannot open database: ") + sqlite3_errmsg(db_));
    }
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
}

bool Database::load_snapshot(const std::string& bytes) {
    if (!bytes.empty()) {
        const sqlite3_int64 size = static_cast<sqlite3_int64>(bytes.size());
        unsigned char* copy = static_cast<unsigned char*>(sqlite3_malloc64(size));
        if (!copy) return false;
        std::memcpy(copy, bytes.data(), bytes.size());
        const int rc =
            sqlite3_deserialize(db_, "main", copy, size, size,
                                SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
        if (rc != SQLITE_OK) {
            init_tables();
            return false;
        }
    }
    init_tables();
    return true;
}

std::string Database::snapshot() const {
    sqlite3_int64 size = 0;
    unsigned char* data = sqlite3_serialize(db_, "main", &size, 0);
    if (!data) throw std::runtime_error("cannot serialize database");
    std::string bytes(reinterpret_cast<const char*>(data), static_cast<size_t>(size));
    sqlite3_free(data);
    return bytes;
}

void Database::mark_source(const std::string& source_file, bool deleted) {
    if (source_file.empty()) return;
    Statement stmt(db_, MARK_SOURCE_SQL);
    bind_text(stmt.handle, 1, source_file);
    sqlite3_bind_int64(stmt.handle, 2, now_ms());
    sqlite3_bind_int(stmt.handle, 3, deleted ? 1 : 0);
    stmt.step();
}

void Database::rebuild_aggregates() {
    sqlite3_exec(db_, "DELETE FROM api_records", nullptr, nullptr, nullptr);
    const char* sql = R"sql(
INSERT INTO api_records
    (date, platform, project, model, type, tokens, cost, unit_price, source_file, currency)
SELECT date, platform, project, model, type,
       SUM(tokens), SUM(cost),
       CASE WHEN SUM(tokens) > 0
            THEN ROUND(SUM(cost) * 1000000.0 / SUM(tokens), 4)
            ELSE MAX(unit_price) END,
       GROUP_CONCAT(DISTINCT source_file), MAX(currency)
FROM record_sources
GROUP BY date, platform, project, model, type
)sql";
    if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("cannot rebuild aggregates: ") +
                                 sqlite3_errmsg(db_));
    }
}

std::string Database::export_dirty() {
    ojson sources = ojson::array();
    Statement query(db_,
                    "SELECT source_file, updated_at, deleted FROM source_state"
                    " WHERE dirty != 0 ORDER BY source_file");
    std::vector<std::tuple<std::string, long long, bool>> states;
    while (query.step()) {
        states.emplace_back(column_text(query.handle, 0), sqlite3_column_int64(query.handle, 1),
                            sqlite3_column_int64(query.handle, 2) != 0);
    }

    for (const auto& [file, updated_at, deleted] : states) {
        ojson source = ojson::object();
        source["source_file"] = file;
        source["updated_at"] = updated_at;
        source["deleted"] = deleted;
        if (!deleted) {
            ojson rows = ojson::array();
            Statement row_query(db_,
                                "SELECT date, platform, project, model, type, tokens, cost,"
                                " unit_price, currency FROM record_sources WHERE source_file = ?"
                                " ORDER BY date, platform, project, model, type");
            bind_text(row_query.handle, 1, file);
            while (row_query.step()) {
                ojson row = ojson::object();
                row["date"] = column_text(row_query.handle, 0);
                row["platform"] = column_text(row_query.handle, 1);
                row["project"] = column_text(row_query.handle, 2);
                row["model"] = column_text(row_query.handle, 3);
                row["type"] = column_text(row_query.handle, 4);
                row["tokens"] = sqlite3_column_int64(row_query.handle, 5);
                row["cost"] = sqlite3_column_double(row_query.handle, 6);
                row["unit_price"] = sqlite3_column_double(row_query.handle, 7);
                row["currency"] = column_text(row_query.handle, 8);
                rows.push_back(std::move(row));
            }
            source["rows"] = std::move(rows);
        }
        sources.push_back(std::move(source));
    }

    ojson out = ojson::object();
    out["sources"] = std::move(sources);
    return out.dump();
}

void Database::apply_state(const std::string& json) {
    const ojson state = ojson::parse(json);
    if (!state.is_object() || !state.contains("sources") || !state["sources"].is_array()) {
        throw std::runtime_error("invalid ledger state");
    }

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    try {
        sqlite3_exec(db_, "DELETE FROM api_records", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "DELETE FROM record_sources", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "DELETE FROM source_state", nullptr, nullptr, nullptr);

        Statement insert_state(db_,
                               "INSERT OR REPLACE INTO source_state"
                               " (source_file, updated_at, deleted, dirty) VALUES (?, ?, ?, 0)");
        Statement insert_row(
            db_,
            "INSERT OR REPLACE INTO record_sources"
            " (date, platform, project, model, type, source_file, tokens, cost, unit_price,"
            " currency) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

        for (const ojson& source : state["sources"]) {
            const std::string file = json_text(source.value("source_file", ojson("")));
            if (file.empty()) continue;
            const bool deleted = json_int(source.value("deleted", ojson(0))) != 0;
            insert_state.reset();
            bind_text(insert_state.handle, 1, file);
            sqlite3_bind_int64(insert_state.handle, 2,
                               json_int(source.value("updated_at", ojson(0))));
            sqlite3_bind_int(insert_state.handle, 3, deleted ? 1 : 0);
            insert_state.step();
            if (deleted || !source.contains("rows") || !source["rows"].is_array()) continue;

            for (const ojson& row : source["rows"]) {
                insert_row.reset();
                bind_text(insert_row.handle, 1, json_text(row.value("date", ojson(""))));
                bind_text(insert_row.handle, 2, json_text(row.value("platform", ojson(""))));
                bind_text(insert_row.handle, 3, json_text(row.value("project", ojson(""))));
                bind_text(insert_row.handle, 4, json_text(row.value("model", ojson(""))));
                bind_text(insert_row.handle, 5, json_text(row.value("type", ojson(""))));
                bind_text(insert_row.handle, 6, file);
                sqlite3_bind_int64(insert_row.handle, 7,
                                   json_int(row.value("tokens", ojson(0))));
                sqlite3_bind_double(insert_row.handle, 8,
                                    json_number(row.value("cost", ojson(0))));
                sqlite3_bind_double(insert_row.handle, 9,
                                    json_number(row.value("unit_price", ojson(0))));
                bind_text(insert_row.handle, 10,
                          json_text(row.value("currency", ojson(""))));
                insert_row.step();
            }
        }

        rebuild_aggregates();
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
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

    for (const char* table : {"api_records", "record_sources"}) {
        bool has_currency = false;
        {
            Statement info(db_, (std::string("PRAGMA table_info(") + table + ")").c_str());
            while (info.step()) {
                if (column_text(info.handle, 1) == "currency") has_currency = true;
            }
        }
        if (!has_currency) {
            const std::string sql = std::string("ALTER TABLE ") + table +
                                    " ADD COLUMN currency TEXT NOT NULL DEFAULT ''";
            if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
                throw std::runtime_error(std::string("cannot add currency column: ") +
                                         sqlite3_errmsg(db_));
            }
        }
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

    if (sqlite3_exec(db_, CREATE_STATE_SQL, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("cannot create source_state: ") +
                                 sqlite3_errmsg(db_));
    }
    {
        Statement backfill(db_,
                           "INSERT OR IGNORE INTO source_state"
                           " (source_file, updated_at, deleted, dirty)"
                           " SELECT DISTINCT source_file, ?, 0, 1 FROM record_sources"
                           " WHERE source_file != ''");
        sqlite3_bind_int64(backfill.handle, 1, now_ms());
        backfill.step();
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
                    " GROUP_CONCAT(DISTINCT source_file), MAX(currency) FROM record_sources"
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
    const std::string currency = column_text(query.handle, 5);
    const double unit_price =
        tokens > 0 ? std::round(cost * 1e6 / static_cast<double>(tokens) * 1e4) / 1e4
                   : max_price;

    Statement upsert(db_, UPSERT_AGGREGATE_SQL);
    bind_key(upsert.handle, key);
    sqlite3_bind_int64(upsert.handle, 6, tokens);
    sqlite3_bind_double(upsert.handle, 7, cost);
    sqlite3_bind_double(upsert.handle, 8, unit_price);
    bind_text(upsert.handle, 9, files);
    bind_text(upsert.handle, 10, currency);
    upsert.step();
}

Database::MergeResult Database::merge_import(const std::vector<ojson>& records,
                                              const std::vector<std::string>& old_files_in) {
    MergeResult result;
    if (records.empty()) return result;
    const std::vector<std::string> old_files = normalize_files(old_files_in);

    struct Total {
        long long tokens = 0;
        double cost = 0.0;
        std::string currency;
    };
    std::map<std::vector<std::string>, Total> totals;
    if (!old_files.empty()) {
        std::string placeholders;
        for (size_t i = 0; i < old_files.size(); ++i) {
            if (i) placeholders += ",";
            placeholders += "?";
        }
        Statement query(db_,
                        ("SELECT date, platform, project, model, type, SUM(tokens), SUM(cost),"
                         " MAX(currency) FROM record_sources WHERE source_file IN (" +
                         placeholders + ") GROUP BY date, platform, project, model, type").c_str());
        for (size_t i = 0; i < old_files.size(); ++i) {
            bind_text(query.handle, static_cast<int>(i) + 1, old_files[i]);
        }
        while (query.step()) {
            std::vector<std::string> key = {
                column_text(query.handle, 0), column_text(query.handle, 1),
                column_text(query.handle, 2), column_text(query.handle, 3),
                column_text(query.handle, 4)};
            totals[std::move(key)] = {sqlite3_column_int64(query.handle, 5),
                                      sqlite3_column_double(query.handle, 6),
                                      column_text(query.handle, 7)};
        }
    }

    std::vector<ojson> winners;
    std::set<std::vector<std::string>> winning_keys;
    for (const ojson& record : records) {
        const std::vector<std::string> key = record_key(record);
        const auto found = totals.find(key);
        if (found == totals.end()) {
            winners.push_back(record);
            winning_keys.insert(key);
            continue;
        }
        const long long tokens = json_int(record.value("tokens", ojson(0)));
        const double cost = json_number(record.value("cost", ojson(0)));
        const double old_cost = found->second.cost;
        const long long old_tokens = found->second.tokens;
        const bool same_values =
            std::abs(cost - old_cost) <= 1e-9 && tokens == old_tokens;
        const std::string currency = json_text(record.value("currency", ojson("")));
        const bool fill_currency =
            same_values && !currency.empty() && found->second.currency.empty();
        const bool higher =
            cost > old_cost + 1e-9 ||
            (std::abs(cost - old_cost) <= 1e-9 && tokens > old_tokens);
        if (higher || fill_currency) {
            winners.push_back(record);
            winning_keys.insert(key);
        } else {
            result.dropped++;
        }
    }
    if (winning_keys.empty()) return result;

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    try {
        std::set<std::string> changed_sources;
        if (!old_files.empty()) {
            std::string placeholders;
            for (size_t i = 0; i < old_files.size(); ++i) {
                if (i) placeholders += ",";
                placeholders += "?";
            }
            for (const auto& key : winning_keys) {
                Statement remove(
                    db_,
                    ("DELETE FROM record_sources WHERE date=? AND platform=? AND project=?"
                     " AND model=? AND type=? AND source_file IN (" + placeholders + ")")
                        .c_str());
                bind_key(remove.handle, key);
                for (size_t i = 0; i < old_files.size(); ++i) {
                    bind_text(remove.handle, 6 + static_cast<int>(i), old_files[i]);
                }
                remove.step();
            }
        }

        Statement source(db_, UPSERT_SOURCE_SQL);
        for (const ojson& record : winners) {
            const std::vector<std::string> key = record_key(record);
            const std::string source_file = json_text(record.value("source_file", ojson("")));
            source.reset();
            bind_key(source.handle, key);
            bind_text(source.handle, 6, source_file);
            sqlite3_bind_int64(source.handle, 7, json_int(record.value("tokens", ojson(0))));
            sqlite3_bind_double(source.handle, 8, json_number(record.value("cost", ojson(0))));
            sqlite3_bind_double(source.handle, 9,
                                json_number(record.value("unit_price", ojson(0))));
            bind_text(source.handle, 10, json_text(record.value("currency", ojson(""))));
            source.step();
            refresh_aggregate(key);
            if (!source_file.empty()) changed_sources.insert(source_file);
        }
        for (const std::string& source_file : changed_sources) {
            mark_source(source_file, false);
        }

        for (const std::string& file : old_files) {
            if (changed_sources.count(file)) continue;
            Statement count(db_, "SELECT COUNT(*) FROM record_sources WHERE source_file = ?");
            bind_text(count.handle, 1, file);
            const bool empty = !count.step() || sqlite3_column_int64(count.handle, 0) == 0;
            mark_source(file, empty);
            if (empty) result.deleted_files++;
        }

        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    result.written = static_cast<int>(winners.size());
    return result;
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

Database::DeleteResult Database::delete_date_range(const std::string& start,
                                                    const std::string& end) {
    DeleteResult result;
    if (start.empty() || end.empty() || start > end) {
        throw std::runtime_error("invalid date range");
    }

    std::vector<std::vector<std::string>> keys;
    {
        Statement query(db_,
                        "SELECT DISTINCT date, platform, project, model, type"
                        " FROM record_sources WHERE date >= ? AND date <= ?");
        bind_text(query.handle, 1, start);
        bind_text(query.handle, 2, end);
        while (query.step()) {
            keys.push_back({column_text(query.handle, 0), column_text(query.handle, 1),
                            column_text(query.handle, 2), column_text(query.handle, 3),
                            column_text(query.handle, 4)});
        }
    }
    if (keys.empty()) return result;

    std::vector<std::string> files;
    {
        Statement query(db_,
                        "SELECT DISTINCT source_file FROM record_sources"
                        " WHERE date >= ? AND date <= ? AND source_file != ''");
        bind_text(query.handle, 1, start);
        bind_text(query.handle, 2, end);
        while (query.step()) files.push_back(column_text(query.handle, 0));
    }

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    try {
        {
            Statement remove(db_,
                             "DELETE FROM record_sources WHERE date >= ? AND date <= ?");
            bind_text(remove.handle, 1, start);
            bind_text(remove.handle, 2, end);
            remove.step();
            result.rows = sqlite3_changes(db_);
        }

        for (const auto& key : keys) refresh_aggregate(key);

        for (const std::string& file : files) {
            Statement count(db_, "SELECT COUNT(*) FROM record_sources WHERE source_file = ?");
            bind_text(count.handle, 1, file);
            const bool empty = !count.step() || sqlite3_column_int64(count.handle, 0) == 0;
            mark_source(file, empty);
        }

        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    return result;
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
        record.currency = column_text(query.handle, 10);
        records.push_back(std::move(record));
    }
    return records;
}
