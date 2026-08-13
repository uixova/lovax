# Lovax build — zero dependencies, single translation unit (src/main.cpp).
# `make` gives you an optimized ./lovax; run scripts with ./lovax file.lov (or
# just ./lovax for the REPL). See `make help` for everything.

CXX      ?= g++
STD      := -std=c++17
SRC      := src/main.cpp
BIN      := lovax
PREFIX   ?= $(HOME)/.local

# Release flags match CI exactly. -fno-gcse/-fno-crossjumping keep the computed-
# goto interpreter dispatch fast (GCC otherwise merges the dispatch tails).
REL_FLAGS  := -O3 -fno-gcse -fno-crossjumping
DEV_FLAGS  := -O0 -g
ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

.DEFAULT_GOAL := $(BIN)
.PHONY: all dev asan clean install uninstall run repl test golden diff \
        jit robustness unit fuzz sandbox embed net stress cover bench help

# ---- builds ----
HEADERS := $(wildcard src/*.hpp src/*/*.hpp)
$(BIN): $(SRC) $(HEADERS)
	$(CXX) $(STD) $(REL_FLAGS) -o $(BIN) $(SRC)

all: $(BIN)

dev: ## fast unoptimized build for iteration (./lovax)
	$(CXX) $(STD) $(DEV_FLAGS) -o $(BIN) $(SRC)

asan: ## AddressSanitizer + UBSan build (./lovax)
	$(CXX) $(STD) $(ASAN_FLAGS) -o $(BIN) $(SRC)

# ---- run ----
run: $(BIN) ## build then run a script:  make run FILE=path/to.lov
	@./$(BIN) $(FILE)

repl: $(BIN) ## build then drop into the REPL
	@./$(BIN)

# ---- install ----
install: $(BIN) ## copy ./lovax to $(PREFIX)/bin (default ~/.local/bin)
	@mkdir -p $(PREFIX)/bin
	@cp $(BIN) $(PREFIX)/bin/$(BIN)
	@echo "installed $(PREFIX)/bin/$(BIN)  (ensure $(PREFIX)/bin is on PATH)"

uninstall:
	@rm -f $(PREFIX)/bin/$(BIN) && echo "removed $(PREFIX)/bin/$(BIN)"

clean:
	@rm -f $(BIN) && echo "cleaned"

# ---- tests (each gate is also runnable on its own) ----
test: $(BIN) golden diff jit robustness unit sandbox embed stress cover ## the correctness + safety suite
	@echo "== all core gates passed =="

golden: $(BIN)
	@./tests/run_tests.sh
diff: $(BIN)
	@./tests/differential.sh
jit: $(BIN)
	@./tests/jit_asm.sh
robustness: $(BIN)
	@./tests/robustness.sh
unit:
	@./tests/unit.sh
fuzz: $(BIN)
	@./tests/fuzz.sh
sandbox: $(BIN)
	@./tests/sandbox.sh
embed:
	@./tests/embed.sh
net: $(BIN)
	@./tests/net_multi.sh
stress: $(BIN)
	@./tests/stress.sh
cover: $(BIN)
	@./tests/trace_coverage.sh
bench: $(BIN)
	@./tests/bench.sh

help: ## show this help
	@echo "Lovax make targets:"
	@echo "  make          build optimized ./lovax (release flags)"
	@echo "  make dev      fast unoptimized build"
	@echo "  make asan     AddressSanitizer + UBSan build"
	@echo "  make run FILE=x.lov   build and run a script"
	@echo "  make repl     build and start the REPL"
	@echo "  make install  copy ./lovax to ~/.local/bin"
	@echo "  make test     full correctness + safety suite"
	@echo "  make golden|diff|jit|robustness|unit|fuzz|sandbox|embed|net|stress|cover|bench"
	@echo "  make clean"
