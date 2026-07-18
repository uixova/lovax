#ifndef LOVAX_MODULE_GAME_HPP
#define LOVAX_MODULE_GAME_HPP

#include "common.hpp"
#include "math.hpp"

namespace Lovax {
namespace StdLib {

// ===== Easing (Penner functions) =====

inline bool easeByName(const std::string& name, double t, double& out) {
    const double PI_ = 3.14159265358979323846;
    if (name == "linear")       { out = t; return true; }
    if (name == "in_quad")      { out = t * t; return true; }
    if (name == "out_quad")     { out = 1 - (1 - t) * (1 - t); return true; }
    if (name == "in_out_quad")  { out = t < 0.5 ? 2*t*t : 1 - std::pow(-2*t + 2, 2) / 2; return true; }
    if (name == "in_cubic")     { out = t * t * t; return true; }
    if (name == "out_cubic")    { out = 1 - std::pow(1 - t, 3); return true; }
    if (name == "in_out_cubic") { out = t < 0.5 ? 4*t*t*t : 1 - std::pow(-2*t + 2, 3) / 2; return true; }
    if (name == "in_sine")      { out = 1 - std::cos(t * PI_ / 2); return true; }
    if (name == "out_sine")     { out = std::sin(t * PI_ / 2); return true; }
    if (name == "in_out_sine")  { out = -(std::cos(PI_ * t) - 1) / 2; return true; }
    if (name == "out_back") {
        const double c1 = 1.70158, c3 = c1 + 1;
        out = 1 + c3 * std::pow(t - 1, 3) + c1 * std::pow(t - 1, 2);
        return true;
    }
    if (name == "out_elastic") {
        if (t == 0) { out = 0; return true; }
        if (t == 1) { out = 1; return true; }
        const double c4 = (2 * PI_) / 3;
        out = std::pow(2, -10 * t) * std::sin((t * 10 - 0.75) * c4) + 1;
        return true;
    }
    if (name == "out_bounce") {
        const double n1 = 7.5625, d1 = 2.75;
        if (t < 1 / d1)        out = n1 * t * t;
        else if (t < 2 / d1)   { t -= 1.5 / d1;   out = n1 * t * t + 0.75; }
        else if (t < 2.5 / d1) { t -= 2.25 / d1;  out = n1 * t * t + 0.9375; }
        else                   { t -= 2.625 / d1; out = n1 * t * t + 0.984375; }
        return true;
    }
    return false;
}

// ===== game module =====

inline ObjPtr makeGameModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    // ease(t, "out_bounce"): Penner easing — t is clamped to [0,1]
    def("ease", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("ease", "2", args.size(), line);
        if (!isNumeric(args[0]) || args[1]->type() != ObjectType::STRING) {
            return makeError("ease(t, name) expects a number and a string", line);
        }
        double t = asDouble(args[0]);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        const std::string& name = static_cast<StringObject*>(args[1].get())->value;
        double out;
        if (!easeByName(name, t, out)) {
            return makeError("unknown easing: \"" + name + "\" (valid: linear, in_quad, out_quad, "
                             "in_out_quad, in_cubic, out_cubic, in_out_cubic, in_sine, out_sine, "
                             "in_out_sine, out_back, out_elastic, out_bounce)", line);
        }
        return makeObj<FloatObject>(out);
    });

    // pick(list): random element
    def("pick", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("pick", "1", args.size(), line);
        if (args[0]->type() != ObjectType::LIST) {
            return makeError("pick() expects a list, got " + typeName(args[0]->type()) + "", line);
        }
        auto* l = static_cast<ListObject*>(args[0].get());
        if (l->elements.empty()) return makeError("pick() cannot pick from an empty list", line);
        return l->elements[(size_t)rngBounded(l->elements.size())];
    });

    // pick_weighted({"sword": 5, "gem": 1}): picks a key by weight (loot tables!)
    def("pick_weighted", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("pick_weighted", "1", args.size(), line);
        if (args[0]->type() != ObjectType::MAP) {
            return makeError("pick_weighted() expects a map: {\"reward\": weight, ...}", line);
        }
        auto* m = static_cast<MapObject*>(args[0].get());
        if (m->entries.empty()) return makeError("pick_weighted() cannot pick from an empty map", line);
        double total = 0;
        for (const auto& e : m->entries) {
            if (!isNumeric(e.second)) {
                return makeError("pick_weighted() weights must be numbers", line);
            }
            double w = asDouble(e.second);
            if (w < 0) return makeError("pick_weighted() weights cannot be negative", line);
            total += w;
        }
        if (total <= 0) return makeError("pick_weighted() total weight must be greater than 0", line);
        double r = rngU53() * total;
        double acc = 0;
        for (const auto& e : m->entries) {
            acc += asDouble(e.second);
            if (r < acc) return e.first;
        }
        return m->entries.back().first;
    });

    // shuffle(list): shuffles in place (Fisher-Yates), returns the list
    def("shuffle", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("shuffle", "1", args.size(), line);
        if (args[0]->type() != ObjectType::LIST) {
            return makeError("shuffle() expects a list, got " + typeName(args[0]->type()) + "", line);
        }
        auto& els = static_cast<ListObject*>(args[0].get())->elements;
        for (size_t i = els.size(); i > 1; --i) {
            std::swap(els[i - 1], els[(size_t)rngBounded(i)]);
        }
        return args[0];
    });

    // signal(): new signal object
    def("signal", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("signal", "0", args.size(), line);
        return makeObj<SignalObject>();
    });

    // connect(signal, fn): adds a listener (the same function is not added twice)
    def("connect", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("connect", "2", args.size(), line);
        if (args[0]->type() != ObjectType::SIGNAL ||
            (args[1]->type() != ObjectType::FUNCTION && args[1]->type() != ObjectType::BUILTIN)) {
            return makeError("connect(signal, fn) expects a signal and a function", line);
        }
        auto* sig = static_cast<SignalObject*>(args[0].get());
        for (const auto& l : sig->listeners) {
            if (l.get() == args[1].get()) return args[0]; // already connected
        }
        gcShade(args[1].get());   // write barrier (RFC-023)
        sig->listeners.push_back(args[1]);
        return args[0];
    });

    // disconnect(signal, fn): removes a listener
    def("disconnect", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("disconnect", "2", args.size(), line);
        if (args[0]->type() != ObjectType::SIGNAL) {
            return makeError("disconnect(signal, fn) expects a signal as its first argument", line);
        }
        auto* sig = static_cast<SignalObject*>(args[0].get());
        for (size_t i = 0; i < sig->listeners.size(); ++i) {
            if (sig->listeners[i].get() == args[1].get()) {
                sig->listeners.erase(sig->listeners.begin() + i);
                return TRUE_OBJ;
            }
        }
        return FALSE_OBJ;
    });

    // emit(signal, ...args): calls every listener in order
    def("emit", [](const Args& args, int line, const CallFn& call) -> ObjPtr {
        if (args.empty()) return argCountError("emit", "at least 1", args.size(), line);
        if (args[0]->type() != ObjectType::SIGNAL) {
            return makeError("emit(signal, ...) expects a signal as its first argument", line);
        }
        auto* sig = static_cast<SignalObject*>(args[0].get());
        Args fwd(args.begin() + 1, args.end());
        // Snapshot copy: stays safe if a listener disconnects during emit
        auto listeners = sig->listeners;
        for (const auto& l : listeners) {
            auto r = call(l, fwd, line);
            if (isError(r)) return r;
        }
        return NULL_OBJ_;
    });

    // ===== Timers (poll-based: checked each frame with timer_done) =====

    auto nowSeconds = []() {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration<double>(now).count();
    };

    // timer(seconds): creates a countdown object (map)
    def("timer", [nowSeconds](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) {
            return makeError("timer(seconds) expects a number", line);
        }
        auto t = makeObj<MapObject>();
        t->set(makeObj<StringObject>("duration"),
               makeObj<FloatObject>(asDouble(args[0])));
        t->set(makeObj<StringObject>("start"),
               makeObj<FloatObject>(nowSeconds()));
        return t;
    });

    auto timerFields = [](const ObjPtr& t, double& süre, double& start) -> bool {
        if (t->type() != ObjectType::MAP) return false;
        auto* m = static_cast<MapObject*>(t.get());
        auto s = m->get(makeObj<StringObject>("duration"));
        auto b = m->get(makeObj<StringObject>("start"));
        if (s == nullptr || b == nullptr || !isNumeric(s) || !isNumeric(b)) return false;
        süre = asDouble(s);
        start = asDouble(b);
        return true;
    };

    // timer_done(t): has the duration elapsed?
    def("timer_done", [nowSeconds, timerFields](const Args& args, int line, const CallFn&) -> ObjPtr {
        double süre, start;
        if (args.size() != 1 || !timerFields(args[0], süre, start)) {
            return makeError("timer_done() expects an object created by timer()", line);
        }
        return boolObj(nowSeconds() - start >= süre);
    });

    // timer_left(t): remaining seconds (never below 0)
    def("timer_left", [nowSeconds, timerFields](const Args& args, int line, const CallFn&) -> ObjPtr {
        double süre, start;
        if (args.size() != 1 || !timerFields(args[0], süre, start)) {
            return makeError("timer_left() expects an object created by timer()", line);
        }
        double left = süre - (nowSeconds() - start);
        return makeObj<FloatObject>(left > 0 ? left : 0.0);
    });

    // timer_reset(t): restarts the countdown (cooldown pattern)
    def("timer_reset", [nowSeconds, timerFields](const Args& args, int line, const CallFn&) -> ObjPtr {
        double süre, start;
        if (args.size() != 1 || !timerFields(args[0], süre, start)) {
            return makeError("timer_reset() expects an object created by timer()", line);
        }
        static_cast<MapObject*>(args[0].get())->set(
            makeObj<StringObject>("start"),
            makeObj<FloatObject>(nowSeconds()));
        return args[0];
    });

    // chance(p): true with probability p (0..1) — game sugar for random() < p
    def("chance", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) return makeError("chance(p) expects a number in 0..1", line);
        double p = asDouble(args[0]);
        return boolObj(rngU53() < p);
    });
    // noise(x[, y]): deterministic value noise in 0..1 (seeded; terrain/jitter)
    def("noise", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.empty() || args.size() > 2 || !isNumeric(args[0]) ||
            (args.size() == 2 && !isNumeric(args[1])))
            return makeError("noise(x[, y]) expects one or two numbers", line);
        auto hash = [](long long n) -> double {
            unsigned long long h = (unsigned long long)n * 2654435761ULL;
            h ^= h >> 15; h *= 0x2545F4914F6CDD1DULL; h ^= h >> 13;
            return (double)(h & 0xFFFFFF) / (double)0xFFFFFF;
        };
        auto smooth = [](double t) { return t * t * (3 - 2 * t); };
        double x = asDouble(args[0]);
        double y = args.size() == 2 ? asDouble(args[1]) : 0.0;
        long long xi = (long long)std::floor(x), yi = (long long)std::floor(y);
        double xf = x - xi, yf = y - yi;
        auto corner = [&](long long cx, long long cy) { return hash(cx * 73856093LL ^ cy * 19349663LL); };
        double n00 = corner(xi, yi), n10 = corner(xi + 1, yi);
        double n01 = corner(xi, yi + 1), n11 = corner(xi + 1, yi + 1);
        double sx = smooth(xf), sy = smooth(yf);
        double a = n00 + (n10 - n00) * sx;
        double b = n01 + (n11 - n01) * sx;
        return makeObj<FloatObject>(a + (b - a) * sy);
    });

    mod->frozen = true;
    mod->moduleName = "game";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}


} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_GAME_HPP
