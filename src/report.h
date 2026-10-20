#pragma once

#include <string>
#include <vector>

#include "db.h"
#include "json.h"

ojson build_report_data(Database& db);
std::string build_html(Database& db);
std::string export_report(Database& db, const std::string& output_path = "");
