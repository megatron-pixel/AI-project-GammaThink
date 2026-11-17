#pragma once

#include <cstddef>
#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

namespace axiom {

struct Evidence {
    std::string archive;
    std::string path;
    std::string title;
    std::string text;
};

struct LibrarySearch {
    std::vector<Evidence> evidence;
    std::vector<std::string> errors;
    std::size_t archives_searched = 0;
    bool complete = true;
};

// Evidence is source text, never an automatically trusted logical premise.
LibrarySearch search_library(const std::filesystem::path& root,
                             const std::string& query,
                             std::size_t max_results = 5,
                             std::size_t max_text_bytes = 4096);
void check_library(const std::filesystem::path& root, std::ostream& output);

}
