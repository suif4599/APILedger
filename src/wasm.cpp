#include <emscripten.h>

#include <cstdlib>
#include <string>

#include "db.h"
#include "importer.h"
#include "json.h"
#include "paths.h"
#include "report.h"

namespace {

Database* g_db = nullptr;
std::string g_result;
std::string g_snapshot;

const char* store(ojson value) {
    g_result = value.dump();
    return g_result.c_str();
}

const char* store_error(const std::string& message) {
    ojson value = ojson::object();
    value["ok"] = false;
    value["error"] = message;
    return store(std::move(value));
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE int apiledger_init() {
    setenv("APILEDGER_SHARE_DIR", "/apiledger", 1);
    setenv("APILEDGER_ROOT", "/apiledger/root", 1);
    paths::init();
    try {
        delete g_db;
        g_db = new Database(":memory:");
        g_db->connect_memory();
        g_db->load_snapshot("");
        return 0;
    } catch (const std::exception& error) {
        delete g_db;
        g_db = nullptr;
        return -1;
    }
}

EMSCRIPTEN_KEEPALIVE int apiledger_load_state(const unsigned char* data, int length) {
    if (!g_db || !data || length <= 0) return -1;
    const std::string bytes(reinterpret_cast<const char*>(data), static_cast<size_t>(length));
    try {
        g_db->close();
        g_db->connect_memory();
        return g_db->load_snapshot(bytes) ? 0 : -1;
    } catch (const std::exception&) {
        return -1;
    }
}

EMSCRIPTEN_KEEPALIVE const char* apiledger_import(const char* name,
                                                  const unsigned char* data, int length) {
    if (!g_db) return store_error("ledger is not initialized");
    try {
        const SourceBundle bundle =
            load_sources_bytes(name ? name : "",
                               std::string(reinterpret_cast<const char*>(data),
                                           static_cast<size_t>(length)));

        ojson files = ojson::array();
        for (const ParsedFile& file : bundle.files) {
            const ReplaceResult result = import_parsed_file(*g_db, file);
            ojson item = ojson::object();
            item["name"] = result.name;
            item["records"] = result.records;
            item["written"] = result.written;
            item["dropped"] = result.dropped;
            item["replaced"] = result.replaced;
            files.push_back(std::move(item));
        }

        ojson skipped = ojson::array();
        for (const SkippedSource& source : bundle.skipped) {
            ojson item = ojson::object();
            item["name"] = source.name;
            item["reason"] = source.reason;
            skipped.push_back(std::move(item));
        }

        ojson out = ojson::object();
        out["ok"] = true;
        out["files"] = std::move(files);
        out["skipped"] = std::move(skipped);
        out["report"] = build_report_data(*g_db);
        return store(std::move(out));
    } catch (const std::exception& error) {
        return store_error(error.what());
    }
}

EMSCRIPTEN_KEEPALIVE const char* apiledger_report_json() {
    if (!g_db) return store_error("ledger is not initialized");
    try {
        return store(build_report_data(*g_db));
    } catch (const std::exception& error) {
        return store_error(error.what());
    }
}

EMSCRIPTEN_KEEPALIVE const char* apiledger_export_dirty() {
    if (!g_db) return store_error("ledger is not initialized");
    try {
        g_result = g_db->export_dirty();
        return g_result.c_str();
    } catch (const std::exception& error) {
        return store_error(error.what());
    }
}

EMSCRIPTEN_KEEPALIVE const char* apiledger_apply_state(const char* json, int length) {
    if (!g_db) return store_error("ledger is not initialized");
    try {
        g_db->apply_state(std::string(json, static_cast<size_t>(length)));
        ojson out = ojson::object();
        out["ok"] = true;
        return store(std::move(out));
    } catch (const std::exception& error) {
        return store_error(error.what());
    }
}

EMSCRIPTEN_KEEPALIVE const char* apiledger_delete_range(const char* start, const char* end) {
    if (!g_db) return store_error("ledger is not initialized");
    try {
        const Database::DeleteResult result =
            g_db->delete_date_range(start ? start : "", end ? end : "");
        ojson out = ojson::object();
        out["ok"] = true;
        out["deleted"] = result.rows;
        return store(std::move(out));
    } catch (const std::exception& error) {
        return store_error(error.what());
    }
}

EMSCRIPTEN_KEEPALIVE const unsigned char* apiledger_snapshot(int* out_length) {
    if (!g_db) return nullptr;
    try {
        g_snapshot = g_db->snapshot();
    } catch (const std::exception&) {
        return nullptr;
    }
    if (out_length) *out_length = static_cast<int>(g_snapshot.size());
    return reinterpret_cast<const unsigned char*>(g_snapshot.data());
}

}  // extern "C"
