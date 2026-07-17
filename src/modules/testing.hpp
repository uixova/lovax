#ifndef LOVAX_MODULE_TESTING_HPP
#define LOVAX_MODULE_TESTING_HPP

#include "common.hpp"

// testing — assertions + a summary, so Lovax code can test Lovax code.
//   use testing
//   testing.assert_eq(topla(2, 2), 4, "toplama")
//   testing.assert_true(x > 0)
//   testing.assert_error(fn() -> 1 / 0, "sifira bolme hata vermeli")
//   exit(testing.summary())     # prints the report; returns the fail count

namespace Lovax {
namespace StdLib {

struct TestState {
    long long passed = 0;
    long long failed = 0;
    std::vector<std::string> failures;
};
inline TestState& testState() { static TestState s; return s; }

inline ObjPtr makeTestingModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    auto label = [](const Args& args, size_t i) -> std::string {
        if (args.size() > i && args[i]->type() == ObjectType::STRING) {
            return static_cast<StringObject*>(args[i].get())->value;
        }
        return "";
    };
    auto record = [](bool ok, const std::string& what, int line) {
        auto& st = testState();
        if (ok) { st.passed++; return; }
        st.failed++;
        st.failures.push_back("line " + std::to_string(line) + ": " + what);
    };

    // assert_eq(actual, expected[, label])
    def("assert_eq", [label, record](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 2 || args.size() > 3) {
            return argCountError("assert_eq", "2-3", args.size(), line);
        }
        bool ok = objectEquals(args[0], args[1]);
        std::string what = label(args, 2);
        if (what.empty()) what = "assert_eq";
        if (!ok) {
            what += " — got " + inspectQuoted(args[0]) + ", expected " + inspectQuoted(args[1]);
        }
        record(ok, what, line);
        return boolObj(ok);
    });

    // assert_true(condition[, label])
    def("assert_true", [label, record](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.empty() || args.size() > 2) {
            return argCountError("assert_true", "1-2", args.size(), line);
        }
        bool ok = objectTruthy(args[0]);
        std::string what = label(args, 1);
        if (what.empty()) what = "assert_true";
        if (!ok) what += " — got " + inspectQuoted(args[0]);
        record(ok, what, line);
        return boolObj(ok);
    });

    // assert_error(fn[, label]): passes when calling fn raises/throws
    def("assert_error", [label, record](const Args& args, int line, const CallFn& call) -> ObjPtr {
        if (args.empty() || args.size() > 2 ||
            (args[0]->type() != ObjectType::FUNCTION && args[0]->type() != ObjectType::BUILTIN)) {
            return makeError("assert_error(fn[, label]) expects a function", line);
        }
        auto r = call(args[0], {}, line);
        bool ok = isError(r);
        std::string what = label(args, 1);
        if (what.empty()) what = "assert_error";
        if (!ok) what += " — no error was raised";
        record(ok, what, line);
        return boolObj(ok);
    });

    // summary(): prints the report, returns the FAIL COUNT (exit-code friendly)
    def("summary", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("summary", "0", args.size(), line);
        auto& st = testState();
        std::cout << "tests: " << st.passed << " passed, " << st.failed << " failed\n";
        for (const auto& f : st.failures) std::cout << "  FAIL " << f << "\n";
        long long fails = st.failed;
        st.passed = st.failed = 0;      // reset so several suites can run in one process
        st.failures.clear();
        return makeObj<IntegerObject>(fails);
    });

    mod->frozen = true;
    mod->moduleName = "testing";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}

} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_TESTING_HPP
