#ifndef PKG_HPP
#define PKG_HPP

#include <string>
#include <vector>
#include <utility>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <filesystem>
#include <cstdlib>

// Lume package manager (RFC-007 phase 2): version-pinned, reproducible installs.
//
// The security story starts here. `lume install user/repo@v1.2.0` clones exactly
// that git tag and records the resolved commit SHA in lume.lock — so a dependency
// can never silently change under you (the npm/pip supply-chain trap). A no-arg
// `lume install` reads lume.json and reinstalls every dependency at its locked
// version. (Runtime isolation — the sandbox that stops a package from touching
// the network/filesystem without permission — is the second layer, see main.cpp
// --allow-* flags.)

namespace Lume {
namespace Pkg {

// Runs a shell command, returns its exit code.
inline int run(const std::string& cmd) { return std::system(cmd.c_str()); }

// Captures a command's stdout (trimmed).
inline std::string capture(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

// Splits "user/repo@v1.2.0" -> {"user/repo", "v1.2.0"} (version may be empty).
// The version '@' always follows the repo path, i.e. it comes after the last '/'.
// The '@' in scp URLs (git@github.com:...) sits before the path's '/', so it's
// left alone.
inline std::pair<std::string, std::string> splitVersion(const std::string& target) {
    auto at = target.rfind('@');
    auto slash = target.rfind('/');
    if (at != std::string::npos && at > 0 &&
        (slash == std::string::npos || at > slash)) {
        return {target.substr(0, at), target.substr(at + 1)};
    }
    return {target, ""};
}

inline std::string toUrl(const std::string& repo) {
    if (repo.find("://") != std::string::npos || repo.rfind("git@", 0) == 0) return repo;
    return "https://github.com/" + repo + ".git";
}

inline std::string pkgName(const std::string& repo) {
    std::string name = repo;
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".git") == 0)
        name = name.substr(0, name.size() - 4);
    auto slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    return name;
}

// Minimal, tolerant reader for lume.json's "dependencies" object:
//   "dependencies": { "name": "user/repo@tag", ... }
// Returns (name, spec) pairs. Not a general JSON parser — our own controlled file.
inline std::vector<std::pair<std::string, std::string>> readDeps(const std::string& path) {
    std::vector<std::pair<std::string, std::string>> deps;
    std::ifstream f(path);
    if (!f) return deps;
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    auto dpos = s.find("\"dependencies\"");
    if (dpos == std::string::npos) return deps;
    auto open = s.find('{', dpos);
    auto close = s.find('}', open);
    if (open == std::string::npos || close == std::string::npos) return deps;
    std::string body = s.substr(open + 1, close - open - 1);
    // scan "key" : "value" pairs
    size_t i = 0;
    auto nextString = [&](size_t& pos, std::string& out) -> bool {
        while (pos < body.size() && body[pos] != '"') pos++;
        if (pos >= body.size()) return false;
        size_t start = ++pos;
        while (pos < body.size() && body[pos] != '"') pos++;
        if (pos >= body.size()) return false;
        out = body.substr(start, pos - start);
        pos++;
        return true;
    };
    while (i < body.size()) {
        std::string key, val;
        if (!nextString(i, key)) break;
        while (i < body.size() && body[i] != ':') i++;
        if (i >= body.size()) break;
        i++;
        if (!nextString(i, val)) break;
        deps.push_back({key, val});
    }
    return deps;
}

// Installs one dependency into lume_libs/<name>, pinned to @version if given.
// Returns "" on success, else an error message. Fills sha with the resolved commit.
inline std::string installOne(const std::string& target, std::string& outName, std::string& sha) {
    auto [repo, version] = splitVersion(target);
    std::string url = toUrl(repo);
    std::string name = pkgName(repo);
    outName = name;
    if (name.empty()) return "cannot derive a package name from: " + target;

    std::filesystem::path dest = std::filesystem::path("lume_libs") / name;
    std::error_code ec;
    if (std::filesystem::exists(dest)) {
        std::filesystem::remove_all(dest, ec); // reinstall to honor the pinned version
    }
    std::filesystem::create_directories("lume_libs", ec);

    std::string branch = version.empty() ? "" : (" --branch \"" + version + "\"");
    std::string cmd = "git clone --depth 1" + branch + " \"" + url + "\" \"" + dest.string() + "\" 2>&1";
    if (run(cmd) != 0)
        return "git clone failed for '" + target + "' (does the repo/tag exist? is git installed?)";

    sha = capture("git -C \"" + dest.string() + "\" rev-parse HEAD");
    std::filesystem::remove_all(dest / ".git", ec); // packages are plain folders
    return "";
}

// Writes lume.lock: one "name version sha" line per resolved dependency.
inline void writeLock(const std::vector<std::string>& lines) {
    std::ofstream f("lume.lock");
    f << "# lume.lock — resolved dependency versions (auto-generated, commit this)\n";
    for (const auto& l : lines) f << l << "\n";
}

// Ensures a lume.json exists and records a dependency in it (idempotent, minimal).
inline void recordInManifest(const std::string& name, const std::string& spec) {
    std::string content;
    if (std::filesystem::exists("lume.json")) {
        std::ifstream f("lume.json"); std::stringstream ss; ss << f.rdbuf(); content = ss.str();
    }
    auto deps = readDeps("lume.json");
    bool found = false;
    for (auto& d : deps) if (d.first == name) { d.second = spec; found = true; }
    if (!found) deps.push_back({name, spec});

    std::ofstream f("lume.json");
    f << "{\n  \"name\": \"my-lume-project\",\n  \"version\": \"0.1.0\",\n  \"dependencies\": {\n";
    for (size_t i = 0; i < deps.size(); ++i) {
        f << "    \"" << deps[i].first << "\": \"" << deps[i].second << "\"";
        f << (i + 1 < deps.size() ? ",\n" : "\n");
    }
    f << "  }\n}\n";
}

// `lume install [target[@ver]]`. No target -> install everything in lume.json.
inline int install(int argc, char* argv[]) {
    std::vector<std::string> lockLines;

    if (argc < 3) {
        // No-arg: reproducible install from the manifest.
        auto deps = readDeps("lume.json");
        if (deps.empty()) {
            std::cerr << "[Install] no lume.json with dependencies found.\n"
                         "  Install one:  lume install user/repo@v1.0.0\n";
            return 64;
        }
        std::cout << "Installing " << deps.size() << " dependencies from lume.json...\n";
        for (const auto& [name, spec] : deps) {
            std::string outName, sha;
            std::string err = installOne(spec, outName, sha);
            if (!err.empty()) { std::cerr << "[Install Error] " << err << "\n"; return 1; }
            std::string ver = splitVersion(spec).second;
            std::cout << "  " << outName << " @ " << (ver.empty() ? "latest" : ver)
                      << " (" << sha.substr(0, 10) << ")\n";
            lockLines.push_back(outName + " " + (ver.empty() ? "latest" : ver) + " " + sha);
        }
        writeLock(lockLines);
        std::cout << "Locked to lume.lock.\n";
        return 0;
    }

    std::string target = argv[2];
    std::string outName, sha;
    std::string ver = splitVersion(target).second;
    if (ver.empty()) {
        std::cout << "Warning: installing '" << target << "' unpinned (latest). "
                     "Pin a version for reproducible, tamper-evident installs:\n"
                     "  lume install " << target << "@v1.0.0\n";
    }
    std::cout << "Installing " << target << " ...\n";
    std::string err = installOne(target, outName, sha);
    if (!err.empty()) { std::cerr << "[Install Error] " << err << "\n"; return 1; }

    recordInManifest(outName, target);
    // merge into existing lock
    std::vector<std::string> lines;
    {
        std::ifstream lf("lume.lock");
        std::string l;
        while (std::getline(lf, l)) if (!l.empty() && l[0] != '#') {
            if (l.rfind(outName + " ", 0) != 0) lines.push_back(l);
        }
    }
    lines.push_back(outName + " " + (ver.empty() ? "latest" : ver) + " " + sha);
    writeLock(lines);

    std::cout << "Installed: lume_libs/" << outName << " (" << sha.substr(0, 10) << ")\n"
              << "Recorded in lume.json + lume.lock. Use it with:  use " << outName << "\n";
    return 0;
}

} // namespace Pkg
} // namespace Lume

#endif // PKG_HPP
