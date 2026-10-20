#pragma once

#include <string>

#include "csv.h"

Table parse_xlsx_bytes(const std::string& bytes);
Table read_xlsx(const std::string& path);
bool is_xlsx_file(const std::string& path);
