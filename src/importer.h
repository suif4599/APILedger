#pragma once

#include <optional>
#include <string>
#include <vector>

#include "db.h"
#include "json.h"
#include "models.h"

struct ParsedFile {
    std::string name;
    std::optional<DateRange> date_range;
    std::vector<ojson> records;
};

struct SkippedSource {
    std::string name;
    std::string reason;
};

struct SourceBundle {
    std::vector<ParsedFile> files;
    std::vector<SkippedSource> skipped;
};

struct ReplaceResult {
    std::string name;
    std::vector<std::string> platforms;
    std::optional<DateRange> date_range;
    std::vector<std::string> replaced;
    int records = 0;
    int deleted = 0;
    int written = 0;
    int dropped = 0;
};

SourceBundle load_sources(const std::string& path);
SourceBundle load_sources_bytes(const std::string& name, const std::string& bytes);
ReplaceResult import_parsed_file(Database& db, const ParsedFile& file);
