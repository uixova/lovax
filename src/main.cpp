#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#if defined(__linux__)
#include <unistd.h>
#endif
#include "lexer/token.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "vm/vm.hpp"
#include "jit/disasm.hpp"
#include "utils/colors.hpp"
#include "pkg.hpp"

static const char* LOVAX_VERSION = "1.2.0";

// ---- single-binary bundling (RFC-027) ---------------------------------------
// `lovax bundle app.lov -o app` copies this interpreter and appends the script as
// a trailer, so the result runs with no separate Lovax install (like Deno compile
// / Bun --compile). Trailer layout at the very end of the file:
//     [ script bytes ][ u64 little-endian script length ][ 16-byte magic ]
// On startup the binary reads its own tail; a matching magic means "run the
// embedded script", so a bundled executable ignores Lovax's own CLI and treats
// its argv as the app's arguments.
static const char LOVAX_BUNDLE_MAGIC[16] = { 'L','O','V','A','X','B','U','N','D','L','E','v','0','0','1' };

// Path to the running executable (for reading our own trailer / copying ourselves).
static std::string selfExePath(const char* argv0) {
#if defined(__linux__)
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return std::string(buf); }
#endif
    return argv0 ? std::string(argv0) : std::string();
}

// If this executable has a bundle trailer, return the embedded script; else "".
static std::string readEmbeddedScript(const char* argv0) {
    std::string path = selfExePath(argv0);
    if (path.empty()) return "";
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    const std::streamoff trailer = (std::streamoff)sizeof(LOVAX_BUNDLE_MAGIC) + 8;
    if (size < trailer) return "";
    char magic[sizeof(LOVAX_BUNDLE_MAGIC)];
    f.seekg(size - (std::streamoff)sizeof(LOVAX_BUNDLE_MAGIC));
    f.read(magic, sizeof(magic));
    if (std::memcmp(magic, LOVAX_BUNDLE_MAGIC, sizeof(magic)) != 0) return "";
    uint64_t len = 0;
    f.seekg(size - trailer);
    unsigned char lenb[8];
    f.read(reinterpret_cast<char*>(lenb), 8);
    for (int i = 0; i < 8; ++i) len |= (uint64_t)lenb[i] << (8 * i);
    if (len == 0 || (std::streamoff)len > size - trailer) return "";
    std::string script(len, '\0');
    f.seekg(size - trailer - (std::streamoff)len);
    f.read(&script[0], (std::streamsize)len);
    return script;
}

// `lovax bundle <script.lov> -o <out>`: self + script + u64 len + magic, +x.
static int writeBundle(int argc, char** argv) {
    std::string scriptPath, outPath;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-o" || a == "--output") && i + 1 < argc) outPath = argv[++i];
        else if (scriptPath.empty()) scriptPath = a;
    }
    if (scriptPath.empty() || outPath.empty()) {
        std::cerr << "usage: lovax bundle <script.lov> -o <output>\n";
        return 64;
    }
    std::string self = selfExePath(argv[0]);
    std::ifstream selff(self, std::ios::binary);
    std::ifstream scriptf(scriptPath, std::ios::binary);
    if (!selff) { std::cerr << "bundle: cannot read interpreter binary\n"; return 1; }
    if (!scriptf) { std::cerr << "bundle: cannot read script '" << scriptPath << "'\n"; return 1; }
    // A bundled binary must not re-bundle its own trailer; strip it if present.
    std::string base((std::istreambuf_iterator<char>(selff)), std::istreambuf_iterator<char>());
    if (!readEmbeddedScript(argv[0]).empty()) {
        std::string embedded = readEmbeddedScript(argv[0]);
        base.resize(base.size() - sizeof(LOVAX_BUNDLE_MAGIC) - 8 - embedded.size());
    }
    std::string script((std::istreambuf_iterator<char>(scriptf)), std::istreambuf_iterator<char>());
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) { std::cerr << "bundle: cannot write '" << outPath << "'\n"; return 1; }
    out.write(base.data(), (std::streamsize)base.size());
    out.write(script.data(), (std::streamsize)script.size());
    uint64_t len = script.size();
    unsigned char lenb[8];
    for (int i = 0; i < 8; ++i) lenb[i] = (unsigned char)(len >> (8 * i));
    out.write(reinterpret_cast<char*>(lenb), 8);
    out.write(LOVAX_BUNDLE_MAGIC, sizeof(LOVAX_BUNDLE_MAGIC));
    out.close();
    std::error_code ec;
    std::filesystem::permissions(outPath,
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
        std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
        std::filesystem::perms::others_exec, ec);
    std::cout << "bundled '" << scriptPath << "' -> '" << outPath << "' ("
              << (base.size() + script.size()) / 1024 << " KB, self-contained)\n";
    return 0;
}

// Evaluate one REPL chunk on a persistent VM. A lone bare expression is echoed
// (wrapped in 'say') so `2 + 3` or `player.hp` print their value like Python.
static void replEval(Lovax::VM& vm, const std::string& src) {
    Lovax::Lexer lexer(src);
    Lovax::Parser parser(lexer);
    auto program = parser.parseProgram();
    if (!parser.errors().empty()) {
        for (const auto& err : parser.errors()) {
            std::cerr << Lovax::Color::errRed() << err.toString()
                      << Lovax::Color::errReset() << "\n";
        }
        return;
    }
    if (program->statements.size() == 1 &&
        program->statements[0]->nodeType() == Lovax::NodeType::EXPRESSION_STATEMENT) {
        auto* es = static_cast<Lovax::ExpressionStatement*>(program->statements[0].get());
        auto say = std::make_unique<Lovax::SayStatement>();
        say->token = es->token;
        say->values.push_back(std::move(es->expression));
        program->statements[0] = std::move(say);
    }
    vm.resetReplState();
    auto result = vm.interpret(program.get());
    if (Lovax::isError(result)) {
        std::cerr << Lovax::Color::errRed() << result->inspect()
                  << Lovax::Color::errReset() << std::endl;
    }
}

// Interactive read-eval-print loop (started when lovax is launched with no script).
// A header line ending in ':' opens a block that is collected until a blank line.
static int runRepl() {
    std::cout << "Lovax " << LOVAX_VERSION << " REPL — type 'exit' to quit, blank line ends a block\n";
    Lovax::VM::setBaseDir(".");
    Lovax::VM vm;
    std::string line, block;
    bool inBlock = false;
    while (true) {
        std::cout << (inBlock ? "... " : ">>> ") << std::flush;
        if (!std::getline(std::cin, line)) { std::cout << "\n"; break; }

        if (!inBlock) {
            std::string trimmed = line;
            size_t a = trimmed.find_first_not_of(" \t");
            if (a == std::string::npos) continue;             // empty line
            std::string t = trimmed.substr(a);
            if (t == "exit" || t == "quit") break;
            // A trailing ':' (block header) starts multi-line collection.
            size_t last = t.find_last_not_of(" \t");
            if (last != std::string::npos && t[last] == ':') {
                block = line + "\n";
                inBlock = true;
                continue;
            }
            replEval(vm, line);
        } else {
            if (line.find_first_not_of(" \t") == std::string::npos) {
                replEval(vm, block);                          // blank line ends block
                block.clear();
                inBlock = false;
            } else {
                block += line + "\n";
            }
        }
    }
    return 0;
}

// Flags that affect how a program is run (parsed from the CLI; all default for a
// bundled app, which just runs with the JIT on).
struct RunOpts {
    bool dumpBytecode = false;
    bool noJit = false, jitRA = false, noRA = false, jitTrace = false,
         noTrace = false, jitNumFn = false, noNumFn = false;
    bool jitStats = false, memStats = false;
};

// Lex, parse, (optionally dump), compile, and run one source string. `scriptArgs`
// must already be populated by the caller. Returns the process exit code.
static int runLovaxSource(const std::string& input, const std::string& baseDir, const RunOpts& o) {
    Lovax::Lexer lexer(input);
    Lovax::Parser parser(lexer);
    auto program = parser.parseProgram();

    if (!parser.errors().empty()) {
        for (const auto& err : parser.errors()) {
            std::cerr << Lovax::Color::errRed() << err.toString()
                      << Lovax::Color::errReset() << "\n";
        }
        std::cerr << parser.errors().size() << " syntax error(s) found; the program was not run."
                  << std::endl;
        return 65; // EX_DATAERR
    }

    if (o.dumpBytecode) {
        Lovax::GlobalTable gt;
        Lovax::Compiler comp(gt);
        try {
            auto proto = comp.compileProgram(program.get());
            std::function<void(const std::shared_ptr<Lovax::Proto>&, const std::string&)> dump =
                [&](const std::shared_ptr<Lovax::Proto>& p, const std::string& name) {
                    Lovax::Jit::disassemble(p->chunk, name);
                    for (const auto& k : p->chunk.consts) {
                        if (k.isObj() && k.asObj() &&
                            k.asObj()->type() == Lovax::ObjectType::RETURN_VALUE) {
                            auto* po = dynamic_cast<Lovax::ProtoObject*>(k.asObj());
                            if (po) dump(po->proto, "fn " + (po->proto->name.empty()
                                                             ? "?" : po->proto->name));
                        }
                    }
                };
            dump(proto, "<script>");
        } catch (const Lovax::CompileError& ce) {
            std::cerr << "[Compile Error] line " << ce.line << ": " << ce.message << "\n";
            return 65;
        }
        return 0;
    }

    Lovax::VM::setBaseDir(baseDir);

    Lovax::VM vm;
#ifdef LOVAX_JIT_ACTIVE
    if (o.noJit) vm.jitEnabled_ = false;
    if (o.jitRA) Lovax::Jit::jitRAEnabled = true;
    if (o.noRA)  Lovax::Jit::jitRAEnabled = false;
    if (o.jitTrace) Lovax::Jit::jitTraceEnabled = true;
    if (o.noTrace)  Lovax::Jit::jitTraceEnabled = false;
    if (o.jitNumFn) Lovax::Jit::jitNumFnEnabled = true;
    if (o.noNumFn)  Lovax::Jit::jitNumFnEnabled = false;
#endif
    auto result = vm.interpret(program.get());

#ifdef LOVAX_JIT_ACTIVE
    if (o.jitStats) {
        std::fprintf(stderr, "[jit] compiled: %zu | blacklisted: %zu | region-enters: %zu\n",
                     vm.jitCompiled_, vm.jitDead_, vm.jitEnters_);
    }
#endif
    if (o.memStats) {
        auto& h = Lovax::Heap::get();
        std::fprintf(stderr,
            "[mem] allocations: %zu | collections: %zu | peak: %.1f MB | "
            "gc total: %.2f ms | max pause: %.3f ms\n",
            h.allocCount, h.collections, h.peakBytes / (1024.0 * 1024.0),
            h.gcNanos / 1e6, h.maxPauseNanos / 1e6);
    }

    if (Lovax::isError(result)) {
        std::cerr << Lovax::Color::errRed() << result->inspect()
                  << Lovax::Color::errReset() << std::endl;
        return 70; // EX_SOFTWARE
    }
    return 0;
}

int main(int argc, char* argv[]) {
    // A bundled binary (see `lovax bundle`) carries an embedded script in its
    // trailer: run it directly, with the whole argv exposed as the app's args and
    // no Lovax CLI parsing (so `./myapp --foo` passes --foo to the app).
    {
        std::string embedded = readEmbeddedScript(argv[0]);
        if (!embedded.empty()) {
            for (int i = 1; i < argc; ++i) Lovax::StdLib::scriptArgs().push_back(argv[i]);
            return runLovaxSource(embedded, ".", RunOpts{});
        }
    }

    if (argc < 2) {
        // No script given: drop into the interactive REPL.
        return runRepl();
    }

    if (std::string(argv[1]) == "bundle") return writeBundle(argc, argv);

    std::string arg = argv[1];
    if (arg == "--version" || arg == "-v") {
        std::cout << "Lovax " << LOVAX_VERSION << std::endl;
        return 0;
    }

    // lovax update [--channel stable|latest] — re-runs the install script, which
    // fetches the newest release binary for this platform and replaces this one.
    if (arg == "update") {
        std::string channel = "stable";
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--channel" && i + 1 < argc) channel = argv[++i];
            else if (a.rfind("--channel=", 0) == 0) channel = a.substr(10);
        }
        std::cout << "Lovax " << LOVAX_VERSION << " — checking for updates (channel: "
                  << channel << ")..." << std::endl;
        // The install script is idempotent: it self-updates in place. Piping it
        // through the shell is the same path a first-time user takes.
        std::string url = "https://raw.githubusercontent.com/uixova/lovax/main/install.sh";
        std::string cmd = "curl -fsSL \"" + url + "\" | LOVAX_CHANNEL=" + channel + " sh";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "[Update Error] could not run the installer "
                         "(need curl + internet). Manual: "
                      << url << std::endl;
            return 1;
        }
        return 0;
    }

    // lovax install [user/repo@version] — version-pinned, reproducible installs
    // (writes lovax.json + lovax.lock). No arg reinstalls everything in lovax.json.
    if (arg == "install") {
        return Lovax::Pkg::install(argc, argv);
    }

    // Capability flags precede the script path: `lovax --sandbox --allow-net app.lov`.
    // Mentioning any permission flag opts into the sandbox (deny-all baseline),
    // then --allow-* grants back exactly what's needed (Deno's model). With no
    // permission flag, everything is allowed (your own script, you trust it).
    int scriptIdx = 1;
    bool anyPerm = false;
    bool memStats = false;
    bool dumpBytecode = false;
    bool noJit = false;
    bool jitStats = false;
    bool jitRA = false;
    bool noRA = false;
    bool jitTrace = false;
    bool noTrace = false;
    bool jitNumFn = false;
    bool noNumFn = false;
    {
        auto& p = Lovax::StdLib::perms();
        // First pass: does any permission flag appear before the script?
        for (int i = 1; i < argc; ++i) {
            std::string f = argv[i];
            if (f == "--sandbox" || f == "--allow-all" || f == "--allow-net" ||
                f == "--allow-read" || f == "--allow-write" || f == "--allow-env" ||
                f == "--allow-run" || f == "--allow-ffi") { anyPerm = true; }
            else if (f == "--mem-stats") { /* not a permission flag; keep scanning */ }
            else break;
        }
        if (anyPerm) { p.net = p.read = p.write = p.env = p.run = p.ffi = false; }
        for (; scriptIdx < argc; ++scriptIdx) {
            std::string f = argv[scriptIdx];
            if (f == "--sandbox") { /* deny-all baseline already applied */ }
            else if (f == "--allow-all") { p.net = p.read = p.write = p.env = p.run = p.ffi = true; }
            else if (f == "--allow-net")   p.net = true;
            else if (f == "--allow-read")  p.read = true;
            else if (f == "--allow-write") p.write = true;
            else if (f == "--allow-env")   p.env = true;
            else if (f == "--allow-run")   p.run = true;
            else if (f == "--allow-ffi")   p.ffi = true;
            else if (f == "--mem-stats")   memStats = true;
            else if (f == "--dump-bytecode") dumpBytecode = true;
            else if (f == "--no-jit")      noJit = true;
            else if (f == "--jit-stats")   jitStats = true;
            else if (f == "--jit-ra")      jitRA = true;
            else if (f == "--no-ra")       noRA = true;
            else if (f == "--jit-trace")   jitTrace = true;
            else if (f == "--no-trace")    noTrace = true;
            else if (f == "--jit-numfn")   jitNumFn = true;
            else if (f == "--no-numfn")    noNumFn = true;
            else break; // first non-flag argument is the script path
        }
    }
    if (scriptIdx >= argc) {
        std::cerr << "[System Error] no script given after permission flags" << std::endl;
        return 64;
    }
    arg = argv[scriptIdx];

    std::ifstream file(arg);
    if (!file.is_open()) {
        std::cerr << "[System Error] cannot open file: " << arg << std::endl;
        return 1;
    }

    // Read the whole file into a string
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string input = buffer.str();

    // Extra CLI arguments after the script path are exposed via os.args().
    for (int i = scriptIdx + 1; i < argc; ++i) {
        Lovax::StdLib::scriptArgs().push_back(argv[i]);
    }

    RunOpts opts;
    opts.dumpBytecode = dumpBytecode;
    opts.noJit = noJit; opts.jitRA = jitRA; opts.noRA = noRA;
    opts.jitTrace = jitTrace; opts.noTrace = noTrace;
    opts.jitNumFn = jitNumFn; opts.noNumFn = noNumFn;
    opts.jitStats = jitStats; opts.memStats = memStats;
    return runLovaxSource(input, std::filesystem::path(arg).parent_path().string(), opts);
}
