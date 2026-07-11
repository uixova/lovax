#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include "lexer/token.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "object/environment.hpp"
#include "evaluator/evaluator.hpp"
#include <filesystem>
#include "evaluator/builtins.hpp"
#include "utils/colors.hpp"

static const char* LUME_VERSION = "0.5.0";

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Lume " << LUME_VERSION << "\n"
                  << "Kullanim: " << argv[0] << " <dosya_yolu.lm>" << std::endl;
        return 1;
    }

    std::string arg = argv[1];
    if (arg == "--version" || arg == "-v") {
        std::cout << "Lume " << LUME_VERSION << std::endl;
        return 0;
    }

    std::ifstream file(arg);
    if (!file.is_open()) {
        std::cerr << "[Sistem Hatasi] Belirtilen dosya acilamadi: " << arg << std::endl;
        return 1;
    }

    // Read the whole file into a string
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string input = buffer.str();

    // Step 1: feed the source code to the lexer for tokenization
    Lume::Lexer lexer(input);

    // Step 2: run the parser to build the AST (Abstract Syntax Tree)
    Lume::Parser parser(lexer);
    auto program = parser.parseProgram();

    // If there are syntax errors, DO NOT run: list them all and exit.
    // (Half-running a broken AST and producing wrong output is the most dangerous behavior.)
    if (!parser.errors().empty()) {
        for (const auto& err : parser.errors()) {
            std::cerr << Lume::Color::errRed() << err.toString()
                      << Lume::Color::errReset() << "\n";
        }
        std::cerr << parser.errors().size() << " sözdizimi hatası bulundu, program çalıştırılmadı."
                  << std::endl;
        return 65; // EX_DATAERR: invalid input data
    }

    // Step 3: create the global scope; only CORE builtins are installed.
    // Built-in libraries (math/game/text/file) are invited with 'use' in the script (RFC-006).
    auto globalEnv = std::make_shared<Lume::Environment>();
    Lume::Builtins::installBuiltins(globalEnv);

    // Relative module paths resolve from the entry script's directory
    Lume::Evaluator::setBaseDir(std::filesystem::path(arg).parent_path().string());

    // Extra CLI arguments after the script path are exposed via os.args()
    for (int i = 2; i < argc; ++i) {
        Lume::StdLib::scriptArgs().push_back(argv[i]);
    }

    // Step 4: run the evaluator
    auto result = Lume::Evaluator::eval(program.get(), globalEnv);

    // Runtime error: print the message and exit with an error code
    if (Lume::isError(result)) {
        std::cerr << Lume::Color::errRed() << result->inspect()
                  << Lume::Color::errReset() << std::endl;
        return 70; // EX_SOFTWARE: runtime error
    }

    return 0;
}
