#include "library.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <libxml/HTMLparser.h>
#include <zim/archive.h>
#include <zim/item.h>
#include <zim/search.h>

namespace axiom {
namespace {

constexpr std::size_t max_page_bytes = 65536;

std::vector<std::filesystem::path> archives_at(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) {
        throw std::runtime_error("Archive root does not exist: " + root.string());
    }
    auto archive_name = [](const std::filesystem::path& path) {
        return path.extension() == ".zim" || path.extension() == ".zimaa";
    };
    std::vector<std::filesystem::path> paths;
    if (std::filesystem::is_regular_file(root)) {
        if (!archive_name(root)) {
            throw std::runtime_error("Expected a .zim or first split .zimaa file");
        }
        paths.push_back(root);
    } else if (std::filesystem::is_directory(root)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && archive_name(entry.path())) {
                paths.push_back(entry.path());
            }
        }
    } else {
        throw std::runtime_error("Archive root is not a file or directory");
    }
    std::sort(paths.begin(), paths.end());
    if (paths.empty()) {
        throw std::runtime_error("No ZIM archives found in: " + root.string());
    }
    return paths;
}

std::string lower(std::string text) {
    for (char& value : text) {
        value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    }
    return text;
}

bool navigation_node(const xmlNode* node) {
    auto* raw = xmlGetProp(node, reinterpret_cast<const xmlChar*>("class"));
    if (raw == nullptr) return false;
    const std::string classes = " " + std::string(reinterpret_cast<const char*>(raw)) + " ";
    xmlFree(raw);
    for (const std::string_view token :
         {"navbox", "sidebar", "toc", "mw-editsection", "mw-jump-link", "vector-menu"}) {
        if (classes.find(" " + std::string(token) + " ") != std::string::npos) return true;
    }
    return false;
}

bool omitted_node(const xmlNode* node) {
    if (node->type == XML_COMMENT_NODE) return true;
    if (node->type != XML_ELEMENT_NODE) return false;
    const std::string_view name(reinterpret_cast<const char*>(node->name));
    return name == "script" || name == "style" || name == "noscript"
        || name == "nav" || name == "header" || name == "footer" || navigation_node(node);
}

void prune_math(xmlNode* node) {
    while (node) {
        auto* next = node->next;
        if (omitted_node(node)) {
            xmlUnlinkNode(node);
            xmlFreeNode(node);
        } else {
            prune_math(node->children);
        }
        node = next;
    }
}

void collect_text(const xmlNode* node, std::string& result) {
    for (; node != nullptr && result.size() < max_page_bytes; node = node->next) {
        if (omitted_node(node)) continue;
        if (node->type == XML_ELEMENT_NODE) {
            const std::string_view name(reinterpret_cast<const char*>(node->name));
            if (name == "math") {
                const auto append = [&](std::string_view bytes) {
                    result.append(bytes.substr(0,max_page_bytes - result.size()));
                };
                const auto alternative = std::unique_ptr<xmlChar,decltype(xmlFree)>(
                    xmlGetProp(node,reinterpret_cast<const xmlChar*>("alttext")),xmlFree);
                if (alternative && *alternative) {
                    append("\\(");
                    append(reinterpret_cast<const char*>(alternative.get()));
                    append("\\) ");
                } else {
                    // Flattening MathML loses fraction, radical and exponent structure.
                    const auto copy = std::unique_ptr<xmlNode,decltype(&xmlFreeNode)>(
                        xmlCopyNode(const_cast<xmlNode*>(node),1),xmlFreeNode);
                    const auto buffer = std::unique_ptr<xmlBuffer,decltype(&xmlBufferFree)>(
                        xmlBufferCreate(),xmlBufferFree);
                    if (!copy || !buffer) throw std::runtime_error("MathML extraction allocation failed");
                    prune_math(copy->children);
                    if (xmlNodeDump(buffer.get(),nullptr,copy.get(),0,0) < 0) {
                        throw std::runtime_error("MathML extraction failed");
                    }
                    append(reinterpret_cast<const char*>(xmlBufferContent(buffer.get())));
                    append(" ");
                }
                continue;
            }
        }
        if (node->type == XML_TEXT_NODE && node->content != nullptr) {
            const auto* content = reinterpret_cast<const char*>(node->content);
            result.append(content, std::min(std::char_traits<char>::length(content),
                                           max_page_bytes - result.size()));
            if (result.size() < max_page_bytes) {
                result.push_back(' ');
            }
        } else {
            collect_text(node->children, result);
            if (node->type == XML_ELEMENT_NODE && result.size() < max_page_bytes) {
                const std::string_view name(reinterpret_cast<const char*>(node->name));
                if (name == "p" || name == "div" || name == "section" || name == "table"
                    || name == "tr" || name == "li" || name == "h1" || name == "h2"
                    || name == "h3") {
                    result.push_back('\n');
                }
            }
        }
    }
}

std::string clean_text(const std::string& raw, const std::string& mimetype) {
    std::string text;
    if (mimetype.starts_with("text/html") || mimetype.starts_with("application/xhtml")) {
        const auto free_document = [](xmlDoc* doc) { xmlFreeDoc(doc); };
        std::unique_ptr<xmlDoc, decltype(free_document)> doc(
            htmlReadMemory(raw.data(), static_cast<int>(raw.size()), nullptr, "UTF-8",
                           HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING),
            free_document);
        if (!doc) {
            throw std::runtime_error("HTML extraction failed");
        }
        collect_text(xmlDocGetRootElement(doc.get()), text);
    } else if (mimetype.starts_with("text/plain")) {
        text = raw;
    } else {
        throw std::runtime_error("Unsupported evidence MIME type: " + mimetype);
    }
    std::string normalized;
    normalized.reserve(text.size());
    bool space = true;
    for (const unsigned char character : text) {
        if (character == '\n') {
            if (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
            if (!normalized.empty() && normalized.back() != '\n') normalized.push_back('\n');
            space = true;
        } else if (std::isspace(character)) {
            if (!space) {
                normalized.push_back(' ');
            }
            space = true;
        } else if (character >= 32) {
            normalized.push_back(static_cast<char>(character));
            space = false;
        }
    }
    if (!normalized.empty() && (normalized.back() == ' ' || normalized.back() == '\n')) {
        normalized.pop_back();
    }
    return normalized;
}

std::string excerpt(const std::string& text, const std::string& query, std::size_t limit) {
    std::size_t match = lower(text).find(lower(query));
    if (match == std::string::npos) {
        const auto end = query.find(' ');
        match = lower(text).find(lower(query.substr(0, end)));
    }
    std::size_t begin = match == std::string::npos || match < limit / 4
        ? 0 : match - limit / 4;
    // Keep UTF-8 continuation bytes attached to their leading codepoint.
    while (begin > 0 && (static_cast<unsigned char>(text[begin]) & 0xc0) == 0x80) {
        --begin;
    }
    std::size_t end = std::min(text.size(), begin + limit);
    while (end > begin && end < text.size()
           && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) {
        --end;
    }
    return text.substr(begin, end - begin);
}

Evidence read_evidence(const std::filesystem::path& filename, zim::Archive& archive,
                       const std::string& path, const std::string& query,
                       std::size_t limit) {
    auto item = archive.getEntryByPath(path).getItem(true);
    const auto size = std::min<zim::size_type>(item.getSize(), max_page_bytes);
    const auto data = item.getData(0, size);
    const std::string raw(data.data(), data.size());
    auto text = excerpt(clean_text(raw, item.getMimetype()), query, limit);
    if (text.empty()) {
        throw std::runtime_error("Retrieved entry contains no supported text");
    }
    return {std::filesystem::absolute(filename).string(), item.getPath(),
            item.getTitle(), std::move(text)};
}

}

LibrarySearch search_library(const std::filesystem::path& root,
                             const std::string& query, std::size_t max_results,
                             std::size_t max_text_bytes) {
    if (query.empty() || query.size() > 1024 || query.find('\0') != std::string::npos) {
        throw std::invalid_argument("Search query must contain 1..1024 non-NUL bytes");
    }
    if (max_results == 0 || max_results > 100
        || max_text_bytes == 0 || max_text_bytes > max_page_bytes) {
        throw std::invalid_argument("Search result or excerpt limit is out of range");
    }
    LibrarySearch output;
    std::vector<zim::Archive> archives;
    std::vector<std::filesystem::path> indexed_paths;
    // Limits libzim's retained decompressed cache, not total process memory.
    zim::setClusterCacheMaxSize(16 * 1024 * 1024);
    for (const auto& path : archives_at(root)) {
        try {
            zim::Archive archive(path.string());
            archive.setDirentCacheMaxSize(128);
            if (!archive.hasFulltextIndex()) {
                output.complete = false;
                output.errors.push_back(path.string() + ": FULLTEXT_INDEX_MISSING");
                continue;
            }
            archives.push_back(std::move(archive));
            indexed_paths.push_back(path);
        } catch (const std::exception& error) {
            output.complete = false;
            output.errors.push_back(path.string() + ": " + error.what());
        }
    }
    if (archives.empty()) return output;
    try {
        // One native search gives corpus-wide relevance. Per-archive percentage
        // scores are normalized independently and must not be merged as ranks.
        zim::Searcher searcher(archives);
        const auto search = searcher.search(zim::Query(query));
        const auto results = search.getResults(0, static_cast<int>(max_results));
        output.archives_searched = archives.size();
        for (auto it = results.begin(); it != results.end(); ++it) {
            const auto index = static_cast<std::size_t>(it.getFileIndex());
            const auto& path = indexed_paths.at(index);
            try {
                output.evidence.push_back(
                    read_evidence(path, archives.at(index), it.getPath(), query, max_text_bytes));
            } catch (const std::exception& error) {
                output.complete = false;
                output.errors.push_back(path.string() + ":" + it.getPath()
                                        + ": " + error.what());
            }
        }
    } catch (const std::exception& error) {
        output.complete = false;
        output.errors.push_back(std::string("Cross-archive search failed: ") + error.what());
    }
    return output;
}

void check_library(const std::filesystem::path& root, std::ostream& output) {
    const auto paths = archives_at(root);
    std::size_t failures = 0;
    for (const auto& path : paths) {
        try {
            zim::Archive archive(path.string());
            output << path << "\tentries=" << archive.getEntryCount()
                   << "\tbytes=" << archive.getFilesize()
                   << "\tfulltext=" << archive.hasFulltextIndex()
                   << "\tchecksum-present=" << archive.hasChecksum()
                   << "\tcontent-integrity=NOT_CHECKED\n";
        } catch (const std::exception& error) {
            ++failures;
            output << path << "\tERROR=" << error.what() << '\n';
        }
    }
    output << "archives=" << paths.size() << " failed=" << failures << '\n';
    if (failures != 0) {
        throw std::runtime_error("One or more archives could not be opened");
    }
}

}
