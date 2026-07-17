#ifndef LOVAX_MODULE_FILE_HPP
#define LOVAX_MODULE_FILE_HPP

#include "common.hpp"

namespace Lovax {
namespace StdLib {

// ===== file module =====

// Quotes a CSV field when needed (contains separator, quote, or newline)
inline std::string csvQuote(const std::string& field, const std::string& sep) {
    bool needs = field.find(sep) != std::string::npos ||
                 field.find('"') != std::string::npos ||
                 field.find('\n') != std::string::npos ||
                 field.find('\r') != std::string::npos;
    if (!needs) return field;
    std::string out = "\"";
    for (char c : field) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

inline ObjPtr makeFileModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    auto pathArg = [](const Args& args, const std::string& fname, int line,
                      std::string& out) -> ObjPtr {
        if (args.empty() || args[0]->type() != ObjectType::STRING) {
            return makeError(fname + "() expects a string path as its first argument", line);
        }
        out = static_cast<StringObject*>(args[0].get())->value;
        return nullptr;
    };

    // exists(path): does the file or directory exist?
    def("exists", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("exists", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "exists", line, path)) return err;
        std::error_code ec;
        return boolObj(std::filesystem::exists(path, ec));
    });

    // read_text(path): reads the file as a string
    def("read_text", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().read, "file read", "--allow-read");
        if (args.size() != 1) return argCountError("read_text", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "read_text", line, path)) return err;
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot open file: " + path, line);
        std::stringstream buf;
        buf << f.rdbuf();
        return makeObj<StringObject>(buf.str());
    });

    // write_text(path, text): writes the string to a file (overwrites)
    def("write_text", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().write, "file write", "--allow-write");
        if (args.size() != 2 || args[1]->type() != ObjectType::STRING) {
            return makeError("write_text(path, text) expects two strings", line);
        }
        std::string path;
        if (auto err = pathArg(args, "write_text", line, path)) return err;
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot write file: " + path, line);
        f << static_cast<StringObject*>(args[1].get())->value;
        return NULL_OBJ_;
    });

    // append_text(path, text): appends to the file (for log files)
    def("append_text", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().write, "file write", "--allow-write");
        if (args.size() != 2 || args[1]->type() != ObjectType::STRING) {
            return makeError("append_text(path, text) expects two strings", line);
        }
        std::string path;
        if (auto err = pathArg(args, "append_text", line, path)) return err;
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (!f.is_open()) return makeError("cannot write file: " + path, line);
        f << static_cast<StringObject*>(args[1].get())->value;
        return NULL_OBJ_;
    });

    // read_lines(path): list of lines (line endings stripped)
    def("read_lines", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().read, "file read", "--allow-read");
        if (args.size() != 1) return argCountError("read_lines", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "read_lines", line, path)) return err;
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot open file: " + path, line);
        auto list = makeObj<ListObject>();
        std::string ln;
        while (std::getline(f, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            list->elements.push_back(makeObj<StringObject>(ln));
        }
        return list;
    });

    // delete_file(path): deletes the file; returns success
    def("delete_file", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().write, "file delete", "--allow-write");
        if (args.size() != 1) return argCountError("delete_file", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "delete_file", line, path)) return err;
        std::error_code ec;
        return boolObj(std::filesystem::remove(path, ec));
    });

    // make_dir(path): creates directories (nested included)
    def("make_dir", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().write, "directory create", "--allow-write");
        if (args.size() != 1) return argCountError("make_dir", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "make_dir", line, path)) return err;
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return boolObj(!ec);
    });

    // list_dir(path): ALPHABETICAL list of names in a directory
    def("list_dir", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().read, "directory listing", "--allow-read");
        if (args.size() != 1) return argCountError("list_dir", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "list_dir", line, path)) return err;
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec)) {
            return makeError("not a directory or does not exist: " + path, line);
        }
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
            names.push_back(entry.path().filename().string());
        }
        std::sort(names.begin(), names.end());
        auto list = makeObj<ListObject>();
        for (const auto& n : names) {
            list->elements.push_back(makeObj<StringObject>(n));
        }
        return list;
    });

    // save_data(path, value): saves the value as JSON (game save system)
    def("save_data", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().write, "file write", "--allow-write");
        if (args.size() != 2) return argCountError("save_data", "2", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "save_data", line, path)) return err;
        std::string json, err;
        if (!jsonWrite(args[1], json, err, 0)) {
            return makeError("save_data() failed: " + err, line);
        }
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot write file: " + path, line);
        f << json << "\n";
        return NULL_OBJ_;
    });

    // load_data(path): parses a JSON file into a Lovax value
    def("load_data", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().read, "file read", "--allow-read");
        if (args.size() != 1) return argCountError("load_data", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "load_data", line, path)) return err;
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot open file: " + path, line);
        std::stringstream buf;
        buf << f.rdbuf();
        std::string content = buf.str();

        JsonParser parser(content);
        ObjPtr result;
        if (!parser.parseValue(result, 0)) {
            return makeError("load_data() could not parse JSON: " + parser.err + " [" + path + "]", line);
        }
        parser.skipWs();
        if (parser.i != content.size()) {
            return makeError("load_data() trailing data after JSON [" + path + "]", line);
        }
        return result;
    });

    // read_bytes(path): reads a binary file as a bytes value.
    // For game assets, .bin saves, etc. Files larger than 10 MB are rejected
    // to avoid allocation storms.
    def("read_bytes", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().read, "file read", "--allow-read");
        if (args.size() != 1) return argCountError("read_bytes", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "read_bytes", line, path)) return err;
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return makeError("cannot open file: " + path, line);
        auto size = f.tellg();
        if (size > 10 * 1024 * 1024) {
            return makeError("read_bytes() file too large (10 MB limit): " + path, line);
        }
        f.seekg(0);
        std::string buf((size_t)size, '\0');
        f.read(&buf[0], size);
        return makeObj<BytesObject>(std::move(buf));
    });

    // write_bytes(path, data): writes bytes (or a list of ints 0-255) as a binary file
    def("write_bytes", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().write, "file write", "--allow-write");
        if (args.size() != 2) return argCountError("write_bytes", "2", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "write_bytes", line, path)) return err;
        if (args[1]->type() == ObjectType::BYTES) {
            const std::string& data = static_cast<BytesObject*>(args[1].get())->data;
            std::ofstream bf(path, std::ios::binary);
            if (!bf.is_open()) return makeError("cannot write file: " + path, line);
            bf.write(data.data(), (std::streamsize)data.size());
            return NULL_OBJ_;
        }
        if (args[1]->type() != ObjectType::LIST) {
            return makeError("write_bytes(path, data) expects bytes or a list of ints", line);
        }
        std::string buf;
        const auto& els = static_cast<ListObject*>(args[1].get())->elements;
        buf.reserve(els.size());
        for (const auto& e : els) {
            if (e->type() != ObjectType::INTEGER) {
                return makeError("write_bytes() elements must be integers (0-255)", line);
            }
            long long v = static_cast<IntegerObject*>(e.get())->value;
            if (v < 0 || v > 255) {
                return makeError("write_bytes() byte out of range: " + std::to_string(v), line);
            }
            buf += (char)(unsigned char)v;
        }
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot write file: " + path, line);
        f << buf;
        return NULL_OBJ_;
    });

    // read_csv(path[, separator]): list of rows; each row is a list of string cells.
    // Supports quoted fields: "a,b", "" escaping, newlines inside quotes.
    def("read_csv", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().read, "file read", "--allow-read");
        if (args.empty() || args.size() > 2) return argCountError("read_csv", "1-2", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "read_csv", line, path)) return err;
        std::string sep = ",";
        if (args.size() == 2) {
            if (args[1]->type() != ObjectType::STRING ||
                static_cast<StringObject*>(args[1].get())->value.size() != 1) {
                return makeError("read_csv() separator must be a single-character string", line);
            }
            sep = static_cast<StringObject*>(args[1].get())->value;
        }
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot open file: " + path, line);
        std::stringstream buf;
        buf << f.rdbuf();
        std::string content = buf.str();

        auto rows = makeObj<ListObject>();
        auto row = makeObj<ListObject>();
        std::string cell;
        bool inQuotes = false;
        size_t i = 0;
        char sc = sep[0];

        auto pushCell = [&]() {
            row->elements.push_back(makeObj<StringObject>(cell));
            cell.clear();
        };
        auto pushRow = [&]() {
            pushCell();
            rows->elements.push_back(row);
            row = makeObj<ListObject>();
        };

        while (i < content.size()) {
            char c = content[i];
            if (inQuotes) {
                if (c == '"') {
                    if (i + 1 < content.size() && content[i + 1] == '"') {
                        cell += '"'; // "" kaçışı
                        i += 2;
                        continue;
                    }
                    inQuotes = false;
                    i++;
                    continue;
                }
                cell += c;
                i++;
                continue;
            }
            if (c == '"' && cell.empty()) { inQuotes = true; i++; continue; }
            if (c == sc) { pushCell(); i++; continue; }
            if (c == '\r') { i++; continue; }
            if (c == '\n') { pushRow(); i++; continue; }
            cell += c;
            i++;
        }
        // Last row (if the file does not end with \n)
        if (!cell.empty() || !row->elements.empty()) pushRow();
        return rows;
    });

    // write_csv(path, rows[, separator]): writes a list of lists as CSV
    def("write_csv", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().write, "file write", "--allow-write");
        if (args.size() < 2 || args.size() > 3) return argCountError("write_csv", "2-3", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "write_csv", line, path)) return err;
        if (args[1]->type() != ObjectType::LIST) {
            return makeError("write_csv() expects a list of rows (list of lists)", line);
        }
        std::string sep = ",";
        if (args.size() == 3) {
            if (args[2]->type() != ObjectType::STRING ||
                static_cast<StringObject*>(args[2].get())->value.size() != 1) {
                return makeError("write_csv() separator must be a single-character string", line);
            }
            sep = static_cast<StringObject*>(args[2].get())->value;
        }
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot write file: " + path, line);
        for (const auto& rowObj : static_cast<ListObject*>(args[1].get())->elements) {
            if (rowObj->type() != ObjectType::LIST) {
                return makeError("write_csv() every row must be a list", line);
            }
            const auto& cells = static_cast<ListObject*>(rowObj.get())->elements;
            for (size_t i = 0; i < cells.size(); ++i) {
                if (i > 0) f << sep;
                f << csvQuote(cells[i]->inspect(), sep);
            }
            f << "\n";
        }
        return NULL_OBJ_;
    });

    // size(path): byte size of a file
    def("size", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("size", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "size", line, path)) return err;
        std::error_code ec;
        auto sz = std::filesystem::file_size(path, ec);
        if (ec) return makeError("cannot stat file: " + path, line);
        return makeObj<IntegerObject>((long long)sz);
    });
    def("is_dir", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("is_dir", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "is_dir", line, path)) return err;
        std::error_code ec;
        return boolObj(std::filesystem::is_directory(path, ec));
    });
    def("copy_file", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().write, "file write", "--allow-write");
        if (args.size() != 2 || args[1]->type() != ObjectType::STRING)
            return makeError("copy_file(from, to) expects two strings", line);
        std::string from;
        if (auto err = pathArg(args, "copy_file", line, from)) return err;
        std::string to = static_cast<StringObject*>(args[1].get())->value;
        std::error_code ec;
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
        return boolObj(!ec);
    });
    def("rename", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().write, "file rename", "--allow-write");
        if (args.size() != 2 || args[1]->type() != ObjectType::STRING)
            return makeError("rename(from, to) expects two strings", line);
        std::string from;
        if (auto err = pathArg(args, "rename", line, from)) return err;
        std::string to = static_cast<StringObject*>(args[1].get())->value;
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
        return boolObj(!ec);
    });

    // glob(pattern): files matching a single-directory wildcard ("saves/*.lov").
    // '*' spans any run, '?' one character — our own matcher, no libc fnmatch.
    def("glob", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().read, "directory listing", "--allow-read");
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("file.glob(pattern) expects a string", line);
        }
        const std::string& pattern = static_cast<StringObject*>(args[0].get())->value;
        auto slash = pattern.find_last_of("/\\");
        std::string dir = slash == std::string::npos ? "." : pattern.substr(0, slash);
        std::string pat = slash == std::string::npos ? pattern : pattern.substr(slash + 1);
        std::function<bool(const char*, const char*)> wild =
            [&wild](const char* p2, const char* t) -> bool {
                for (; *p2; ++p2, ++t) {
                    if (*p2 == '*') {
                        while (*p2 == '*') ++p2;
                        if (!*p2) return true;
                        for (; *t; ++t) if (wild(p2, t)) return true;
                        return false;
                    }
                    if (!*t || (*p2 != '?' && *p2 != *t)) return false;
                }
                return !*t;
            };
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            std::string name = entry.path().filename().string();
            if (wild(pat.c_str(), name.c_str())) {
                out->elements.push_back(makeObj<StringObject>(entry.path().string()));
            }
        }
        if (ec) return makeError("file.glob() cannot read directory: " + dir, line);
        std::sort(out->elements.begin(), out->elements.end(),
                  [](const ObjPtr& a, const ObjPtr& b) {
                      return static_cast<StringObject*>(a.get())->value <
                             static_cast<StringObject*>(b.get())->value;
                  });
        return out;
    });

    // walk(dir): every file under dir, recursively (sorted, files only)
    def("walk", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().read, "directory listing", "--allow-read");
        if (args.size() != 1) return argCountError("walk", "1", args.size(), line);
        std::string dir;
        if (auto err = pathArg(args, "walk", line, dir)) return err;
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        std::error_code ec;
        size_t count = 0;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec)) {
                out->elements.push_back(makeObj<StringObject>(it->path().string()));
                if (++count > 100000) return makeError("file.walk() too many files (limit 100k)", line);
            }
        }
        std::sort(out->elements.begin(), out->elements.end(),
                  [](const ObjPtr& a, const ObjPtr& b) {
                      return static_cast<StringObject*>(a.get())->value <
                             static_cast<StringObject*>(b.get())->value;
                  });
        return out;
    });

    mod->frozen = true;
    mod->moduleName = "file";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}


} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_FILE_HPP
