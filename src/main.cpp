#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <cstdlib>
#include <filesystem>
#include "lexer/token.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "vm/vm.hpp"
#include "utils/colors.hpp"
#include "pkg.hpp"

static const char* LUME_VERSION = "0.9.0";

// Evaluate one REPL chunk on a persistent VM. A lone bare expression is echoed
// (wrapped in 'say') so `2 + 3` or `player.hp` print their value like Python.
static void replEval(Lume::VM& vm, const std::string& src) {
    Lume::Lexer lexer(src);
    Lume::Parser parser(lexer);
    auto program = parser.parseProgram();
    if (!parser.errors().empty()) {
        for (const auto& err : parser.errors()) {
            std::cerr << Lume::Color::errRed() << err.toString()
                      << Lume::Color::errReset() << "\n";
        }
        return;
    }
    if (program->statements.size() == 1 &&
        program->statements[0]->nodeType() == Lume::NodeType::EXPRESSION_STATEMENT) {
        auto* es = static_cast<Lume::ExpressionStatement*>(program->statements[0].get());
        auto say = std::make_unique<Lume::SayStatement>();
        say->token = es->token;
        say->values.push_back(std::move(es->expression));
        program->statements[0] = std::move(say);
    }
    vm.resetReplState();
    auto result = vm.interpret(program.get());
    if (Lume::isError(result)) {
        std::cerr << Lume::Color::errRed() << result->inspect()
                  << Lume::Color::errReset() << std::endl;
    }
}

// Interactive read-eval-print loop (started when lume is launched with no script).
// A header line ending in ':' opens a block that is collected until a blank line.
static int runRepl() {
    std::cout << "Lume " << LUME_VERSION << " REPL — type 'exit' to quit, blank line ends a block\n";
    Lume::VM::setBaseDir(".");
    Lume::VM vm;
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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        // No script given: drop into the interactive REPL.
        return runRepl();
    }

    std::string arg = argv[1];
    if (arg == "--version" || arg == "-v") {
        std::cout << "Lume " << LUME_VERSION << std::endl;
        return 0;
    }

    // lume update [--channel stable|latest] — re-runs the install script, which
    // fetches the newest release binary for this platform and replaces this one.
    if (arg == "update") {
        std::string channel = "stable";
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--channel" && i + 1 < argc) channel = argv[++i];
            else if (a.rfind("--channel=", 0) == 0) channel = a.substr(10);
        }
        std::cout << "Lume " << LUME_VERSION << " — checking for updates (channel: "
                  << channel << ")..." << std::endl;
        // The install script is idempotent: it self-updates in place. Piping it
        // through the shell is the same path a first-time user takes.
        std::string url = "https://raw.githubusercontent.com/uixova/lume/main/install.sh";
        std::string cmd = "curl -fsSL \"" + url + "\" | LUME_CHANNEL=" + channel + " sh";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "[Update Error] could not run the installer "
                         "(need curl + internet). Manual: "
                      << url << std::endl;
            return 1;
        }
        return 0;
    }

    // lume install [user/repo@version] — version-pinned, reproducible installs
    // (writes lume.json + lume.lock). No arg reinstalls everything in lume.json.
    if (arg == "install") {
        return Lume::Pkg::install(argc, argv);
    }

    // Capability flags precede the script path: `lume --sandbox --allow-net app.lm`.
    // Mentioning any permission flag opts into the sandbox (deny-all baseline),
    // then --allow-* grants back exactly what's needed (Deno's model). With no
    // permission flag, everything is allowed (your own script, you trust it).
    int scriptIdx = 1;
    bool anyPerm = false;
    {
        auto& p = Lume::StdLib::perms();
        // First pass: does any permission flag appear before the script?
        for (int i = 1; i < argc; ++i) {
            std::string f = argv[i];
            if (f == "--sandbox" || f == "--allow-all" || f == "--allow-net" ||
                f == "--allow-read" || f == "--allow-write" || f == "--allow-env" ||
                f == "--allow-run") { anyPerm = true; }
            else break;
        }
        if (anyPerm) { p.net = p.read = p.write = p.env = p.run = false; }
        for (; scriptIdx < argc; ++scriptIdx) {
            std::string f = argv[scriptIdx];
            if (f == "--sandbox") { /* deny-all baseline already applied */ }
            else if (f == "--allow-all") { p.net = p.read = p.write = p.env = p.run = true; }
            else if (f == "--allow-net")   p.net = true;
            else if (f == "--allow-read")  p.read = true;
            else if (f == "--allow-write") p.write = true;
            else if (f == "--allow-env")   p.env = true;
            else if (f == "--allow-run")   p.run = true;
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

    // Step 1: tokenize.  Step 2: parse into an AST.
    Lume::Lexer lexer(input);
    Lume::Parser parser(lexer);
    auto program = parser.parseProgram();

    // If there are syntax errors, DO NOT run: list them all and exit.
    if (!parser.errors().empty()) {
        for (const auto& err : parser.errors()) {
            std::cerr << Lume::Color::errRed() << err.toString()
                      << Lume::Color::errReset() << "\n";
        }
        std::cerr << parser.errors().size() << " syntax error(s) found; the program was not run."
                  << std::endl;
        return 65; // EX_DATAERR
    }

    // Relative module paths resolve from the entry script's directory.
    Lume::VM::setBaseDir(std::filesystem::path(arg).parent_path().string());

    // Extra CLI arguments after the script path are exposed via os.args()
    for (int i = scriptIdx + 1; i < argc; ++i) {
        Lume::StdLib::scriptArgs().push_back(argv[i]);
    }

    // Step 3: compile to bytecode and run on the VM.
    Lume::VM vm;
    auto result = vm.interpret(program.get());

    if (Lume::isError(result)) {
        std::cerr << Lume::Color::errRed() << result->inspect()
                  << Lume::Color::errReset() << std::endl;
        return 70; // EX_SOFTWARE
    }

    return 0;
}
