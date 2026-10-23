#pragma once

#include <string>
#include <vector>

struct ZipEntry {
    std::string name;
    std::string bytes;
};

std::vector<ZipEntry> read_zip_entries(const std::string& path);
std::vector<ZipEntry> read_zip_entries_from_mem(const std::string& data);
