#include "zip.h"

#include <stdexcept>

#include "miniz/miniz.h"
#include "util.h"

std::vector<ZipEntry> read_zip_entries(const std::string& path) {
    return read_zip_entries_from_mem(util::read_file(path));
}

std::vector<ZipEntry> read_zip_entries_from_mem(const std::string& data) {
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, data.data(), data.size(), 0)) {
        throw std::runtime_error("cannot open zip archive");
    }

    std::vector<ZipEntry> entries;
    const unsigned count = mz_zip_reader_get_num_files(&archive);
    for (unsigned i = 0; i < count; ++i) {
        if (mz_zip_reader_is_file_a_directory(&archive, i)) continue;

        char name[1024];
        if (mz_zip_reader_get_filename(&archive, i, name, sizeof(name)) == 0) continue;
        const std::string entry_name(name);

        size_t size = 0;
        void* bytes = mz_zip_reader_extract_to_heap(&archive, i, &size, 0);
        if (!bytes) continue;
        entries.push_back({entry_name, std::string(static_cast<char*>(bytes), size)});
        mz_free(bytes);
    }

    mz_zip_reader_end(&archive);
    return entries;
}
