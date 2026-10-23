#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <optional>
#include <streambuf>
#include <string>
#include <vector>

#include "db.h"
#include "importer.h"
#include "paths.h"
#include "report.h"
#include "util.h"

namespace {

// Escape non-ASCII bytes so program output stays ASCII even when input data is not.
class AsciiStreamBuffer : public std::streambuf {
public:
    explicit AsciiStreamBuffer(std::streambuf* destination) : destination_(destination) {}

protected:
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) {
            return traits_type::not_eof(ch);
        }
        write_byte(traits_type::to_char_type(ch));
        return ch;
    }

    std::streamsize xsputn(const char* data, std::streamsize count) override {
        for (std::streamsize i = 0; i < count; ++i) {
            write_byte(data[i]);
        }
        return count;
    }

    int sync() override { return destination_->pubsync(); }

private:
    std::streamsize write_byte(char ch) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte < 0x80) {
            destination_->sputc(ch);
            return 1;
        }
        char escape[8];
        const int length = std::snprintf(escape, sizeof(escape), "\\x%02X", byte);
        for (int i = 0; i < length; ++i) destination_->sputc(escape[i]);
        return length;
    }

    std::streambuf* destination_;
};

struct ImportStats {
    int files = 0;
    int records = 0;
    int replaced = 0;
};

void print_skipped(const SourceBundle& bundle) {
    for (const SkippedSource& skipped : bundle.skipped) {
        std::cout << "  [skipped] " << skipped.name << ": " << skipped.reason << "\n";
    }
}

void print_import_summary(const ImportStats& stats) {
    std::cout << "==================================================\n";
    std::string summary = "Import complete: " + std::to_string(stats.files) +
                          " file(s) | " + std::to_string(stats.records) + " row(s) written";
    if (stats.replaced > 0) {
        summary += " | " + std::to_string(stats.replaced) + " overlapping bill(s) merged";
    }
    std::cout << summary << "\n";
}

void open_in_browser(const std::string& path) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    const std::string url = "file://" + normalized;

    const pid_t pid = fork();
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
        _exit(1);
    }
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

void print_usage() {
    std::cout
        << "usage: apiledger <bill.zip|bill.csv|bill.xlsx> [-o <report.html>]\n\n"
        << "Import a zip archive of bills and generate the HTML report.\n"
        << "Without -o the report opens in the default browser.\n\n"
        << "options:\n"
        << "  -o <file>    write the report to <file> without opening it\n"
        << "  -h, --help   show this help\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::cout.rdbuf(new AsciiStreamBuffer(std::cout.rdbuf()));
    std::cerr.rdbuf(new AsciiStreamBuffer(std::cerr.rdbuf()));

    std::optional<std::string> output_path;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "error: -o requires a file path\n";
                return 2;
            }
            output_path = argv[++i];
        } else if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        } else if (!argument.empty() && argument[0] == '-') {
            std::cerr << "error: unrecognized argument: " << argument << "\n\n";
            print_usage();
            return 2;
        } else {
            positional.push_back(argument);
        }
    }

    if (positional.empty()) {
        std::cerr << "error: missing bill archive\n\n";
        print_usage();
        return 2;
    }
    if (positional.size() > 1) {
        std::cerr << "error: expected exactly one bill archive\n";
        return 2;
    }

    paths::init();
    const std::string source = positional.front();

    try {
        Database db(paths::layout().db_path);
        db.connect();

        const SourceBundle bundle = load_sources(source);
        if (bundle.files.empty() && !bundle.skipped.empty()) {
            std::cout << "No importable bill files.\n";
            print_skipped(bundle);
        }

        ImportStats stats;
        stats.files = static_cast<int>(bundle.files.size());
        for (const ParsedFile& file : bundle.files) {
            const ReplaceResult result = import_parsed_file(db, file);
            stats.records += result.records;
            stats.replaced += static_cast<int>(result.replaced.size());
            std::cout << "  wrote " << result.written << " row(s)\n\n";
        }

        print_skipped(bundle);
        print_import_summary(stats);

        const std::string path = export_report(db, output_path.value_or(""));
        std::cout << "\nReport generated: " << path << "\n";
        if (!output_path) open_in_browser(path);

        db.close();
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
