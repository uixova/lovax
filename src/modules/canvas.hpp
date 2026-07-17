#ifndef LOVAX_MODULE_CANVAS_HPP
#define LOVAX_MODULE_CANVAS_HPP

#include "common.hpp"
#include "../utils/colors.hpp"

namespace Lovax {
namespace StdLib {

// ===== canvas module (terminal renderer) =====
// A canvas is a map holding width/height plus a code-point buffer and a color
// buffer. Playable ASCII/roguelike graphics before the real engine exists.
namespace CanvasImpl {
    inline const std::unordered_map<std::string, std::string>& colorCodes() {
        static const std::unordered_map<std::string, std::string> m = {
            {"black","30"},{"red","31"},{"green","32"},{"yellow","33"},
            {"blue","34"},{"magenta","35"},{"cyan","36"},{"white","37"},{"gray","90"}
        };
        return m;
    }
    inline MapObject* asCanvas(const ObjPtr& v) {
        if (v->type() != ObjectType::MAP) return nullptr;
        auto* m = static_cast<MapObject*>(v.get());
        if (m->get(makeObj<StringObject>("__canvas__")) == nullptr) return nullptr;
        return m;
    }
    inline long long dim(MapObject* c, const char* k) {
        auto v = c->get(makeObj<StringObject>(k));
        return v && v->type() == ObjectType::INTEGER ? static_cast<IntegerObject*>(v.get())->value : 0;
    }
    inline ListObject* cbuf(MapObject* c) {
        return static_cast<ListObject*>(c->get(makeObj<StringObject>("buf")).get());
    }
    inline ListObject* ccol(MapObject* c) {
        return static_cast<ListObject*>(c->get(makeObj<StringObject>("col")).get());
    }
}

inline ObjPtr makeCanvasModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;
    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };
    using CanvasImpl::asCanvas;

    // create(w, h): a blank canvas filled with spaces
    def("create", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[0]->type() != ObjectType::INTEGER || args[1]->type() != ObjectType::INTEGER)
            return makeError("create(w, h) expects two integers", line);
        long long w = static_cast<IntegerObject*>(args[0].get())->value;
        long long h = static_cast<IntegerObject*>(args[1].get())->value;
        if (w <= 0 || h <= 0 || w > 1000 || h > 1000) return makeError("create() size out of range (1..1000)", line);
        auto c = makeObj<MapObject>();
        c->set(strKey("__canvas__"), TRUE_OBJ);
        c->set(strKey("w"), makeObj<IntegerObject>(w));
        c->set(strKey("h"), makeObj<IntegerObject>(h));
        auto b = makeObj<ListObject>();
        auto col = makeObj<ListObject>();
        for (long long i = 0; i < w * h; ++i) {
            b->elements.push_back(makeObj<StringObject>(" "));
            col->elements.push_back(makeObj<StringObject>("white"));
        }
        c->set(strKey("buf"), b);
        c->set(strKey("col"), col);
        return c;
    });
    auto putCell = [](MapObject* c, long long x, long long y, const std::string& ch, const std::string& color) {
        long long w = CanvasImpl::dim(c, "w"), h = CanvasImpl::dim(c, "h");
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        long long idx = y * w + x;
        static_cast<StringObject*>(CanvasImpl::cbuf(c)->elements[idx].get())->value = ch;
        static_cast<StringObject*>(CanvasImpl::ccol(c)->elements[idx].get())->value = color;
    };
    // put(canvas, x, y, char[, color])
    def("put", [putCell](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 4 || args.size() > 5) return argCountError("put", "4-5", args.size(), line);
        auto* c = asCanvas(args[0]);
        if (!c || args[1]->type() != ObjectType::INTEGER || args[2]->type() != ObjectType::INTEGER ||
            args[3]->type() != ObjectType::STRING)
            return makeError("put(canvas, x, y, char[, color]) - bad arguments", line);
        std::string color = args.size() == 5 && args[4]->type() == ObjectType::STRING
            ? static_cast<StringObject*>(args[4].get())->value : "white";
        std::string ch = static_cast<StringObject*>(args[3].get())->value;
        if (utf8Length(ch) != 1) return makeError("put() char must be exactly one character", line);
        putCell(c, static_cast<IntegerObject*>(args[1].get())->value,
                   static_cast<IntegerObject*>(args[2].get())->value, ch, color);
        return args[0];
    });
    // write(canvas, x, y, text[, color]): draws a string left to right
    def("write", [putCell](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 4 || args.size() > 5) return argCountError("write", "4-5", args.size(), line);
        auto* c = asCanvas(args[0]);
        if (!c || args[1]->type() != ObjectType::INTEGER || args[2]->type() != ObjectType::INTEGER ||
            args[3]->type() != ObjectType::STRING)
            return makeError("write(canvas, x, y, text[, color]) - bad arguments", line);
        std::string color = args.size() == 5 && args[4]->type() == ObjectType::STRING
            ? static_cast<StringObject*>(args[4].get())->value : "white";
        const std::string& text = static_cast<StringObject*>(args[3].get())->value;
        long long x = static_cast<IntegerObject*>(args[1].get())->value;
        long long y = static_cast<IntegerObject*>(args[2].get())->value;
        size_t i = 0; long long dx = 0;
        while (i < text.size()) {
            int len = utf8CharLen((unsigned char)text[i]);
            putCell(c, x + dx, y, text.substr(i, len), color);
            i += len; dx++;
        }
        return args[0];
    });
    // fill(canvas, char[, color]): fills the whole canvas
    def("fill", [putCell](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 2 || args.size() > 3) return argCountError("fill", "2-3", args.size(), line);
        auto* c = asCanvas(args[0]);
        if (!c || args[1]->type() != ObjectType::STRING) return makeError("fill(canvas, char[, color]) - bad arguments", line);
        std::string color = args.size() == 3 && args[2]->type() == ObjectType::STRING
            ? static_cast<StringObject*>(args[2].get())->value : "white";
        std::string ch = static_cast<StringObject*>(args[1].get())->value;
        long long w = CanvasImpl::dim(c, "w"), h = CanvasImpl::dim(c, "h");
        for (long long y = 0; y < h; ++y) for (long long x = 0; x < w; ++x) putCell(c, x, y, ch, color);
        return args[0];
    });
    // rect / fill_rect(canvas, x, y, w, h, char[, color])
    auto rectFn = [putCell](bool filled) {
        return [putCell, filled](const Args& args, int line, const CallFn&) -> ObjPtr {
            std::string fname = filled ? "fill_rect" : "rect";
            if (args.size() < 6 || args.size() > 7) return argCountError(fname, "6-7", args.size(), line);
            auto* c = asCanvas(args[0]);
            for (int i = 1; i <= 4; ++i) if (args[i]->type() != ObjectType::INTEGER) return makeError(fname + "() coords must be integers", line);
            if (!c || args[5]->type() != ObjectType::STRING) return makeError(fname + "() - bad arguments", line);
            std::string color = args.size() == 7 && args[6]->type() == ObjectType::STRING
                ? static_cast<StringObject*>(args[6].get())->value : "white";
            std::string ch = static_cast<StringObject*>(args[5].get())->value;
            long long x = static_cast<IntegerObject*>(args[1].get())->value;
            long long y = static_cast<IntegerObject*>(args[2].get())->value;
            long long rw = static_cast<IntegerObject*>(args[3].get())->value;
            long long rh = static_cast<IntegerObject*>(args[4].get())->value;
            for (long long j = 0; j < rh; ++j) for (long long i = 0; i < rw; ++i) {
                bool edge = (i == 0 || j == 0 || i == rw - 1 || j == rh - 1);
                if (filled || edge) putCell(c, x + i, y + j, ch, color);
            }
            return args[0];
        };
    };
    def("rect", rectFn(false));
    def("fill_rect", rectFn(true));
    // line(canvas, x1, y1, x2, y2, char[, color]): Bresenham
    def("line", [putCell](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 6 || args.size() > 7) return argCountError("line", "6-7", args.size(), line);
        auto* c = asCanvas(args[0]);
        for (int i = 1; i <= 4; ++i) if (args[i]->type() != ObjectType::INTEGER) return makeError("line() coords must be integers", line);
        if (!c || args[5]->type() != ObjectType::STRING) return makeError("line() - bad arguments", line);
        std::string color = args.size() == 7 && args[6]->type() == ObjectType::STRING
            ? static_cast<StringObject*>(args[6].get())->value : "white";
        std::string ch = static_cast<StringObject*>(args[5].get())->value;
        long long x1 = static_cast<IntegerObject*>(args[1].get())->value, y1 = static_cast<IntegerObject*>(args[2].get())->value;
        long long x2 = static_cast<IntegerObject*>(args[3].get())->value, y2 = static_cast<IntegerObject*>(args[4].get())->value;
        long long dx = std::llabs(x2 - x1), dy = -std::llabs(y2 - y1);
        long long sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1, err = dx + dy;
        while (true) {
            putCell(c, x1, y1, ch, color);
            if (x1 == x2 && y1 == y2) break;
            long long e2 = 2 * err;
            if (e2 >= dy) { err += dy; x1 += sx; }
            if (e2 <= dx) { err += dx; y1 += sy; }
        }
        return args[0];
    });
    // circle(canvas, cx, cy, r, char[, color]): midpoint outline
    def("circle", [putCell](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 5 || args.size() > 6) return argCountError("circle", "5-6", args.size(), line);
        auto* c = asCanvas(args[0]);
        for (int i = 1; i <= 3; ++i) if (args[i]->type() != ObjectType::INTEGER) return makeError("circle() coords must be integers", line);
        if (!c || args[4]->type() != ObjectType::STRING) return makeError("circle() - bad arguments", line);
        std::string color = args.size() == 6 && args[5]->type() == ObjectType::STRING
            ? static_cast<StringObject*>(args[5].get())->value : "white";
        std::string ch = static_cast<StringObject*>(args[4].get())->value;
        long long cx = static_cast<IntegerObject*>(args[1].get())->value, cy = static_cast<IntegerObject*>(args[2].get())->value;
        long long r = static_cast<IntegerObject*>(args[3].get())->value;
        long long x = r, y = 0, err = 1 - r;
        while (x >= y) {
            putCell(c, cx + x, cy + y, ch, color); putCell(c, cx - x, cy + y, ch, color);
            putCell(c, cx + x, cy - y, ch, color); putCell(c, cx - x, cy - y, ch, color);
            putCell(c, cx + y, cy + x, ch, color); putCell(c, cx - y, cy + x, ch, color);
            putCell(c, cx + y, cy - x, ch, color); putCell(c, cx - y, cy - x, ch, color);
            y++;
            if (err < 0) err += 2 * y + 1;
            else { x--; err += 2 * (y - x) + 1; }
        }
        return args[0];
    });
    // render(canvas): prints the canvas with ANSI colors (plain when not a TTY)
    def("render", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("render", "1", args.size(), line);
        auto* c = asCanvas(args[0]);
        if (!c) return makeError("render() expects a canvas", line);
        long long w = CanvasImpl::dim(c, "w"), h = CanvasImpl::dim(c, "h");
        auto* b = CanvasImpl::cbuf(c); auto* col = CanvasImpl::ccol(c);
        bool tty = Color::stdoutIsTTY();
        std::string out;
        for (long long y = 0; y < h; ++y) {
            for (long long x = 0; x < w; ++x) {
                long long i = y * w + x;
                const std::string& ch = static_cast<StringObject*>(b->elements[i].get())->value;
                if (tty) {
                    const std::string& cn = static_cast<StringObject*>(col->elements[i].get())->value;
                    auto it = CanvasImpl::colorCodes().find(cn);
                    if (it != CanvasImpl::colorCodes().end()) out += "\033[1;" + it->second + "m" + ch + "\033[0m";
                    else out += ch;
                } else out += ch;
            }
            out += "\n";
        }
        std::cout << out;
        return NULL_OBJ_;
    });
    // clear_screen(): moves the cursor home + clears (ANSI)
    def("clear_screen", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("clear_screen", "0", args.size(), line);
        if (Color::stdoutIsTTY()) std::cout << "\033[2J\033[H";
        return NULL_OBJ_;
    });

    mod->frozen = true;
    mod->moduleName = "canvas";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}


// Built-in module registry: use <name> looks up here

} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_CANVAS_HPP
