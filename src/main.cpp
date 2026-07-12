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

static const char* LUME_VERSION = "0.6.0";

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Lume " << LUME_VERSION << "\n"
                  << "Usage: " << argv[0] << " <script.lm> [args...]" << std::endl;
        return 1;
    }

    std::string arg = argv[1];
    if (arg == "--version" || arg == "-v") {
        std::cout << "Lume " << LUME_VERSION << std::endl;
        return 0;
    }

    // lume install <user/repo | git-url> — fetches a package into ./lume_libs/<name>
    // (uses the system git; a native registry client is planned)
    if (arg == "install") {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " install <user/repo | git-url>" << std::endl;
            return 64; // EX_USAGE
        }
        std::string target = argv[2];
        std::string url = target;
        if (url.find("://") == std::string::npos && url.rfind("git@", 0) != 0) {
            url = "https://github.com/" + target + ".git"; // user/repo shorthand
        }
        std::string name = url;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".git") == 0) {
            name = name.substr(0, name.size() - 4);
        }
        auto slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        if (name.empty()) {
            std::cerr << "[Install Error] cannot derive a package name from: " << target << std::endl;
            return 64;
        }

        std::filesystem::path dest = std::filesystem::path("lume_libs") / name;
        if (std::filesystem::exists(dest)) {
            std::cerr << "[Install Error] already installed: " << dest.string()
                      << " (delete the folder to reinstall)" << std::endl;
            return 1;
        }
        std::error_code ec;
        std::filesystem::create_directories("lume_libs", ec);

        std::string cmd = "git clone --depth 1 \"" + url + "\" \"" + dest.string() + "\"";
        std::cout << "Installing " << name << " from " << url << " ..." << std::endl;
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "[Install Error] git clone failed (is git installed? does the repo exist?)"
                      << std::endl;
            return 1;
        }
        std::filesystem::remove_all(dest / ".git", ec); // packages are plain folders, not repos
        std::cout << "Installed: " << dest.string() << "\n"
                  << "Use it with:  use " << name << std::endl;
        return 0;
    }

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
    for (int i = 2; i < argc; ++i) {
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
