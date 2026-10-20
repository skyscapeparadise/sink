#include "toml_edit.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#endif
#include <toml.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cctype>
#include <sstream>

namespace toml_edit {
namespace {

// ---- Text positions ------------------------------------------------------------
//
// toml++ reports 1-based lines and 1-based columns counted in characters (not
// bytes), with end positions exclusive.

struct Text {
    std::string s;
    std::vector<size_t> starts;  // byte offset of each line's first character

    explicit Text(std::string text) : s(std::move(text)) {
        starts.push_back(0);
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n') starts.push_back(i + 1);
        }
    }
    int line_count() const {
        // A final newline doesn't start a line of its own.
        return static_cast<int>(starts.size()) - (!s.empty() && s.back() == '\n' ? 1 : 0);
    }
    size_t line_begin(int line) const { return starts[static_cast<size_t>(line - 1)]; }
    size_t line_end(int line) const {  // offset of the '\n' ending it, or of the end of the text
        size_t next = static_cast<size_t>(line) < starts.size() ? starts[static_cast<size_t>(line)] : s.size() + 1;
        return next - 1;
    }
    std::string line(int n) const {
        if (n < 1 || n > line_count()) return std::string();
        size_t b = line_begin(n), e = std::min(line_end(n), s.size());
        return s.substr(b, e - b);
    }
    size_t offset(const toml::source_position& p) const {
        size_t o = line_begin(static_cast<int>(p.line));
        for (uint32_t col = 1; col < p.column && o < s.size(); ++col) {
            ++o;
            while (o < s.size() && (static_cast<unsigned char>(s[o]) & 0xC0) == 0x80) ++o;
        }
        return o;
    }
};

bool is_blank(const std::string& line) {
    return std::all_of(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); });
}

bool is_comment(const std::string& line) {
    size_t i = line.find_first_not_of(" \t");
    return i != std::string::npos && line[i] == '#';
}

bool only_space_or_comment(const std::string& s) {
    size_t i = s.find_first_not_of(" \t\r");
    return i == std::string::npos || s[i] == '#';
}

std::string leading_space(const std::string& line) {
    return line.substr(0, line.find_first_not_of(" \t") == std::string::npos ? 0 : line.find_first_not_of(" \t"));
}

// ---- Keys and headers -----------------------------------------------------------

std::string quote_key(const std::string& key) {
    bool bare = !key.empty() && std::all_of(key.begin(), key.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
    if (bare) return key;
    std::string out = "\"";
    for (unsigned char c : key) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7F) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04X", c); out += buf; }
        else out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string header_text(const Path& path) {
    std::string out = "[";
    for (size_t i = 0; i < path.size(); ++i) {
        if (i) out += ".";
        out += quote_key(path[i]);
    }
    return out + "]";
}

struct Header {
    int line = 0;
    Path path;
    bool array = false;     // [[array of tables]]
    size_t open = 0;        // byte index of the first '[' within the line
    size_t close_end = 0;   // byte index just past the closing ']' (or ']]')
};

// Parses a table header line. Escapes in quoted keys are decoded only as far
// as sink's own names need; a key using anything else simply won't match,
// which makes the edit decline rather than go wrong.
std::optional<Header> parse_header(const std::string& line, int line_no) {
    Header h;
    h.line = line_no;
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line[i] != '[') return std::nullopt;
    h.open = i++;
    if (i < line.size() && line[i] == '[') { h.array = true; ++i; }
    for (;;) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) return std::nullopt;
        std::string key;
        if (line[i] == '"') {
            ++i;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    char e = line[i + 1];
                    if (e == '"' || e == '\\') key += e;
                    else if (e == 'n') key += '\n';
                    else if (e == 't') key += '\t';
                    else return std::nullopt;
                    i += 2;
                } else {
                    key += line[i++];
                }
            }
            if (i >= line.size()) return std::nullopt;
            ++i;
        } else if (line[i] == '\'') {
            size_t end = line.find('\'', i + 1);
            if (end == std::string::npos) return std::nullopt;
            key = line.substr(i + 1, end - i - 1);
            i = end + 1;
        } else {
            while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_' || line[i] == '-')) {
                key += line[i++];
            }
            if (key.empty()) return std::nullopt;
        }
        h.path.push_back(key);
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i < line.size() && line[i] == '.') { ++i; continue; }
        if (i < line.size() && line[i] == ']') {
            ++i;
            if (h.array) {
                if (i >= line.size() || line[i] != ']') return std::nullopt;
                ++i;
            }
            h.close_end = i;
            if (!only_space_or_comment(line.substr(i))) return std::nullopt;
            return h;
        }
        return std::nullopt;
    }
}

bool has_prefix(const Path& p, const Path& prefix) {
    return p.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), p.begin());
}

// Every header line in the document. Candidates come from toml++'s table
// positions rather than scanning every line, so a line inside a multi-line
// string that happens to start with '[' isn't mistaken for one.
void collect_header_lines(const toml::table& t, std::vector<int>& lines) {
    for (auto&& [key, node] : t) {
        (void)key;
        if (const auto* sub = node.as_table()) {
            if (!sub->is_inline()) lines.push_back(static_cast<int>(sub->source().begin.line));
            collect_header_lines(*sub, lines);
        } else if (const auto* arr = node.as_array()) {
            for (auto&& el : *arr) {
                if (const auto* st = el.as_table()) {
                    lines.push_back(static_cast<int>(st->source().begin.line));
                    collect_header_lines(*st, lines);
                }
            }
        }
    }
}

std::vector<Header> headers(const Text& text, const toml::table& doc) {
    std::vector<int> lines;
    collect_header_lines(doc, lines);
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    std::vector<Header> out;
    for (int n : lines) {
        if (auto h = parse_header(text.line(n), n)) out.push_back(*h);
    }
    return out;
}

const Header* explicit_header(const std::vector<Header>& hs, const Path& path) {
    for (const Header& h : hs) {
        if (!h.array && h.path == path) return &h;
    }
    return nullptr;
}

// The last line belonging to the table whose header is `h`: the line before
// the next header, of any table.
int table_last_line(const Text& text, const std::vector<Header>& hs, const Header& h) {
    for (const Header& other : hs) {
        if (other.line > h.line) return other.line - 1;
    }
    return text.line_count();
}

// The last line of the whole subtree under `h`: the line before the next
// header that isn't one of its descendants.
int subtree_last_line(const Text& text, const std::vector<Header>& hs, const Header& h) {
    for (const Header& other : hs) {
        if (other.line > h.line && !has_prefix(other.path, h.path)) return other.line - 1;
    }
    return text.line_count();
}

const toml::table* find_table(const toml::table& doc, const Path& path) {
    const toml::table* t = &doc;
    for (const std::string& seg : path) {
        t = t->get_as<toml::table>(seg);
        if (!t) return nullptr;
    }
    return t;
}

// Whether every node under `t` was written between lines `first` and `last`.
bool within(const toml::table& t, int first, int last) {
    for (auto&& [key, node] : t) {
        int kb = static_cast<int>(key.source().begin.line);
        if (kb && (kb < first || kb > last)) return false;
        const auto& src = node.source();
        if (src.begin.line && (static_cast<int>(src.begin.line) < first || static_cast<int>(src.end.line) > last)) {
            return false;
        }
        if (const auto* sub = node.as_table(); sub && !within(*sub, first, last)) return false;
    }
    return true;
}

std::string with_final_newline(std::string s) {
    if (!s.empty() && s.back() != '\n') s += '\n';
    return s;
}

// Inserts whole lines after line `after` (0 = at the very top).
std::string insert_lines(const Text& text, int after, const std::string& lines) {
    std::string s = with_final_newline(text.s);
    size_t at = after <= 0 ? 0 : std::min(text.line_end(after) + 1, s.size());
    return s.substr(0, at) + lines + s.substr(at);
}

std::string erase_lines(const Text& text, int first, int last) {
    size_t b = text.line_begin(first);
    size_t e = std::min(text.line_end(last) + 1, text.s.size());
    return text.s.substr(0, b) + text.s.substr(e);
}

// The last line in [first, last] holding something other than whitespace or
// a comment; 0 if there is none.
int last_content_line(const Text& text, int first, int last) {
    for (int n = last; n >= first; --n) {
        std::string l = text.line(n);
        if (!is_blank(l) && !is_comment(l)) return n;
    }
    return 0;
}

// ---- The edits, on text ------------------------------------------------------------

std::optional<std::string> set_in_place(const Text& text, const toml::table& doc, const Edit& e) {
    const std::vector<Header> hs = headers(text, doc);
    const toml::table* t = find_table(doc, e.table);
    const std::string kv = quote_key(e.key) + " = " + e.value_text;

    if (t && t->is_inline()) return std::nullopt;

    if (t) {
        if (const toml::node* n = t->get(e.key)) {
            if (n->is_table() || (n->is_array() && n->as_array()->is_array_of_tables())) return std::nullopt;
            const auto& src = n->source();
            size_t b = text.offset(src.begin), en = text.offset(src.end);
            return text.s.substr(0, b) + e.value_text + text.s.substr(en);
        }
    }

    if (e.table.empty()) {
        // A top-level key must come before the first header.
        int first_header = hs.empty() ? text.line_count() + 1 : hs.front().line;
        int after = last_content_line(text, 1, first_header - 1);
        if (after) return insert_lines(text, after, kv + "\n");
        if (hs.empty()) return with_final_newline(text.s) + kv + "\n";
        return insert_lines(text, first_header - 1, kv + "\n\n");
    }

    if (const Header* h = explicit_header(hs, e.table)) {
        int last = table_last_line(text, hs, *h);
        int after = last_content_line(text, h->line + 1, last);
        std::string indent = after ? leading_space(text.line(after)) : std::string();
        return insert_lines(text, after ? after : h->line, indent + kv + "\n");
    }

    // No header of its own. A table defined only through dotted keys or as
    // part of an inline table can't take a header now; one that exists only
    // because deeper tables were declared can.
    if (t) {
        for (auto&& [key, node] : *t) {
            (void)key;
            if (!node.is_table()) return std::nullopt;
        }
    }
    std::string s = with_final_newline(text.s);
    if (!s.empty() && !is_blank(text.line(text.line_count()))) s += "\n";
    return s + header_text(e.table) + "\n" + kv + "\n";
}

std::optional<std::string> add_table_in_place(const Text& text, const toml::table& doc, const Edit& e) {
    if (e.table.empty()) return std::nullopt;
    if (find_table(doc, e.table)) return text.s;
    std::string s = with_final_newline(text.s);
    if (!s.empty() && !is_blank(text.line(text.line_count()))) s += "\n";
    return s + header_text(e.table) + "\n";
}

std::optional<std::string> remove_in_place(const Text& text, const toml::table& doc, const Edit& e) {
    const toml::table* t = find_table(doc, e.table);
    if (!t) return text.s;
    const toml::node* n = nullptr;
    toml::source_region key_src;
    for (auto&& [key, node] : *t) {
        if (key.str() == e.key) { n = &node; key_src = key.source(); }
    }
    if (!n) return text.s;
    if (n->is_table() || !key_src.begin.line) return std::nullopt;

    const int first = static_cast<int>(key_src.begin.line);
    const int last = static_cast<int>(n->source().end.line);
    const std::string first_line = text.line(first);
    const size_t key_at = text.offset(key_src.begin) - text.line_begin(first);
    if (!is_blank(first_line.substr(0, key_at))) return std::nullopt;
    const size_t value_end = text.offset(n->source().end) - text.line_begin(last);
    if (!only_space_or_comment(text.line(last).substr(value_end))) return std::nullopt;
    return erase_lines(text, first, last);
}

std::optional<std::string> remove_table_in_place(const Text& text, const toml::table& doc, const Edit& e) {
    const std::vector<Header> hs = headers(text, doc);
    const toml::table* t = find_table(doc, e.table);
    if (!t) return text.s;
    const Header* h = explicit_header(hs, e.table);
    if (!h) return std::nullopt;
    int last = subtree_last_line(text, hs, *h);
    if (!within(*t, h->line, last)) return std::nullopt;

    // Comments directly above the next header describe it, not this table.
    if (last < text.line_count()) {
        while (last > h->line && is_comment(text.line(last))) --last;
    }
    // Leave the blank lines that separated this table from what follows, and
    // take one from above instead, so the spacing around the gap stays even.
    while (last > h->line && is_blank(text.line(last))) --last;
    // Comments directly above this table's header describe it, so they go.
    int first = h->line;
    while (first > 1 && is_comment(text.line(first - 1))) --first;
    if (first > 1 && is_blank(text.line(first - 1))) --first;
    return erase_lines(text, first, last);
}

std::optional<std::string> rename_table_in_place(const Text& text, const toml::table& doc, const Edit& e) {
    if (e.table.empty() || e.new_name.empty()) return std::nullopt;
    const std::vector<Header> hs = headers(text, doc);
    if (!explicit_header(hs, e.table)) return std::nullopt;

    Path renamed = e.table;
    renamed.back() = e.new_name;
    std::string out = text.s;
    // From the bottom up, so earlier offsets stay valid.
    for (auto it = hs.rbegin(); it != hs.rend(); ++it) {
        if (!has_prefix(it->path, e.table)) continue;
        if (it->array) return std::nullopt;
        Path p = renamed;
        p.insert(p.end(), it->path.begin() + static_cast<long>(e.table.size()), it->path.end());
        const size_t b = text.line_begin(it->line);
        out = out.substr(0, b + it->open) + header_text(p) + out.substr(b + it->close_end);
    }
    return out;
}

// ---- The same edits, on the parsed document ------------------------------------------
//
// Used to check the text edits (the edited text must parse to exactly this)
// and to regenerate when they decline.

bool apply_to_doc(toml::table& doc, const Edit& e) {
    toml::table* t = &doc;
    toml::table* parent = nullptr;
    for (const std::string& seg : e.table) {
        parent = t;
        toml::node* n = t->get(seg);
        if (!n) {
            if (e.kind != Edit::Kind::Set && e.kind != Edit::Kind::AddTable) {
                return true;  // nothing to remove or rename
            }
            t->insert(seg, toml::table{});
            n = t->get(seg);
        }
        t = n->as_table();
        if (!t) return false;
    }
    switch (e.kind) {
        case Edit::Kind::Set: {
            toml::table tmp;
            try {
                tmp = toml::parse("v = " + e.value_text);
            } catch (const toml::parse_error&) {
                return false;
            }
            const toml::node* v = tmp.get("v");
            if (!v) return false;
            v->visit([&](auto&& concrete) { t->insert_or_assign(e.key, concrete); });
            return true;
        }
        case Edit::Kind::AddTable:
            return true;  // created on the way down, if it wasn't there
        case Edit::Kind::Remove:
            t->erase(e.key);
            return true;
        case Edit::Kind::RemoveTable:
            if (!parent) return false;
            parent->erase(e.table.back());
            return true;
        case Edit::Kind::RenameTable: {
            if (!parent || parent->contains(e.new_name)) return false;
            toml::table moved = *t;
            parent->erase(e.table.back());
            parent->insert(e.new_name, std::move(moved));
            return true;
        }
    }
    return false;
}

std::optional<toml::table> parse(const std::string& text) {
    try {
        return toml::parse(text);
    } catch (const toml::parse_error&) {
        return std::nullopt;
    }
}

} // namespace

std::optional<std::string> apply(const std::string& text, const Edit& edit) {
    std::optional<toml::table> doc = parse(text);
    if (!doc) return std::nullopt;
    toml::table expected = *doc;
    if (!apply_to_doc(expected, edit)) return std::nullopt;

    Text t(text);
    std::optional<std::string> out;
    switch (edit.kind) {
        case Edit::Kind::Set: out = set_in_place(t, *doc, edit); break;
        case Edit::Kind::AddTable: out = add_table_in_place(t, *doc, edit); break;
        case Edit::Kind::Remove: out = remove_in_place(t, *doc, edit); break;
        case Edit::Kind::RemoveTable: out = remove_table_in_place(t, *doc, edit); break;
        case Edit::Kind::RenameTable: out = rename_table_in_place(t, *doc, edit); break;
    }
    if (!out) return std::nullopt;

    // The edit must have done exactly what was asked and nothing else.
    std::optional<toml::table> check = parse(*out);
    if (!check || !(*check == expected)) return std::nullopt;
    return out;
}

std::optional<std::string> apply_or_regenerate(const std::string& text, const Edit& edit, bool& regenerated) {
    regenerated = false;
    if (auto out = apply(text, edit)) return out;
    std::optional<toml::table> doc = parse(text);
    if (!doc) return std::nullopt;
    if (!apply_to_doc(*doc, edit)) return std::nullopt;
    regenerated = true;
    std::ostringstream o;
    o << toml::toml_formatter{ *doc } << "\n";
    return o.str();
}

} // namespace toml_edit
