// C++ unit tests for VM internals — the pieces golden tests cannot reach
// directly (GC accounting, shapes, the regex engine's compiler, civil-date
// math). Zero framework: assert-style checks, exit code = failure count.
//
// Build & run:  g++ -std=c++17 -O1 -o unit_tests tests/unit/unit_tests.cpp && ./unit_tests
#include "../../src/vm/vm.hpp"
#include <cstdio>

using namespace Lovax;

static int fails = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

static void testValueSize() {
#ifdef LOVAX_NANBOX
    CHECK(sizeof(Value) == 8);           // RFC-024: NaN-boxed value, one register
#else
    CHECK(sizeof(Value) == 16);          // RFC-013: portable 16-byte tagged union
#endif
}

static void testObjectEquals() {
    auto a = makeObj<IntegerObject>(5);
    auto b = makeObj<FloatObject>(5.0);
    CHECK(objectEquals(a, b));           // 5 == 5.0 across types
    auto t1 = makeObj<TupleObject>();
    t1->elements.push_back(makeObj<IntegerObject>(1));
    auto l1 = makeObj<ListObject>();
    l1->elements.push_back(makeObj<IntegerObject>(1));
    CHECK(!objectEquals(Ref<Object>(t1.get()), Ref<Object>(l1.get()))); // tuple != list
}

static void testMapTypedIndexes() {
    auto m = makeObj<MapObject>();
    auto k = makeObj<StringObject>("hp");
    m->set(k, makeObj<IntegerObject>(100));
    CHECK(m->getStr("hp") != nullptr);
    CHECK(m->findStr("yok") == MapObject::NPOS);
    m->remove(k);
    CHECK(m->getStr("hp") == nullptr);
}

static void testGcBytesPayloadAware() {
    auto s = makeObj<StringObject>(std::string(1000, 'x'));
    CHECK(s->gcBytes() >= 1000);         // payload counted, not just the header
    auto l = makeObj<ListObject>();
    l->elements.resize(100);
    CHECK(l->gcBytes() >= 100 * sizeof(Ref<Object>));
}

static void testCivilDateRoundTrip() {
    using namespace StdLib;
    long long y, m, d;
    dtCivilFromDays(dtDaysFromCivil(2026, 7, 17), y, m, d);
    CHECK(y == 2026 && m == 7 && d == 17);
    CHECK(dtDaysFromCivil(1970, 1, 1) == 0);
    dtCivilFromDays(dtDaysFromCivil(2024, 2, 29), y, m, d);   // leap day survives
    CHECK(y == 2024 && m == 2 && d == 29);
    dtCivilFromDays(-1, y, m, d);                              // pre-epoch
    CHECK(y == 1969 && m == 12 && d == 31);
}

static void testRegexCompiler() {
    using namespace StdLib;
    {
        Rx::Compiler c("a(b|c)+d");
        CHECK(c.compile());
        Rx::Matcher m(c.prog, std::string("abcbd"));
        CHECK(m.matchAt(0));
        CHECK(m.caps[0] == 0 && m.caps[1] == 5);
    }
    {
        Rx::Compiler c("(a");
        CHECK(!c.compile());              // clean compile error, no crash
        CHECK(!c.prog.error.empty());
    }
    {
        Rx::Compiler c("a{2000}");        // over the repetition bound
        CHECK(!c.compile());
    }
}

static void testDeterministicRng() {
    using namespace Builtins;
    rng().seed(42);
    double a = rngU53();
    long long b = rngBounded(1000);
    rng().seed(42);
    CHECK(rngU53() == a);                 // bit-identical replay
    CHECK(rngBounded(1000) == b);
    CHECK(b >= 0 && b < 1000);
}

int main() {
    // The GC needs no VM here: allocation works with collection disabled.
    Heap::get().enabled = false;
    testValueSize();
    testObjectEquals();
    testMapTypedIndexes();
    testGcBytesPayloadAware();
    testCivilDateRoundTrip();
    testRegexCompiler();
    testDeterministicRng();
    if (fails == 0) std::printf("unit: all checks passed\n");
    else            std::printf("unit: %d check(s) FAILED\n", fails);
    return fails;
}
