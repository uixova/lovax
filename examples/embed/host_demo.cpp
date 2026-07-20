// Headless embedding demo (RFC-025). A pure-C++/stdio host — no graphics, no
// dependencies — that embeds Lovax the way a game engine would:
//   * registers native functions and a native `engine` module,
//   * loads game.lov,
//   * drives on_frame(dt) in a fixed-step loop (host -> script),
//   * hands script code opaque native handles (Entity*) and gets them back,
//   * keeps a script value alive across a GC with a Persistent root,
//   * proves owning-handle finalizers free the C++ objects.
//
// Build: g++ -std=c++17 -O2 examples/embed/host_demo.cpp -o host_demo
// Run:   ./host_demo examples/embed/game.lov

#include <cstdio>
#include <cstdint>
#include "../../src/embed/embed.hpp"

using namespace Lovax;
using namespace Lovax::Embed;

// ---- the engine's own C++ world ----
struct Entity { int id; double x = 0, y = 0; };
static int g_liveEntities = 0;   // spawned - freed; must return to 0
static int g_nextId = 1;
static const uint32_t ENTITY_TYPE = 0xE1;   // handle type tag

// Owning-handle finalizer: the GC calls this when the script drops an Entity
// handle. Runs during sweep — no allocation, no VM re-entry (just delete).
static void freeEntity(void* p) {
    delete static_cast<Entity*>(p);
    --g_liveEntities;
}

int main(int argc, char** argv) {
    const char* scriptPath = argc > 1 ? argv[1] : "examples/embed/game.lov";
    Host rt;

    // native global: log(msg)
    rt.native("log", [](const std::vector<Ref<Object>>& a, int line,
                        const BuiltinObject::CallFn&) -> Ref<Object> {
        std::string s;
        if (a.empty() || !asString(a[0], s))
            return makeError("log(msg) expects a string", line);
        std::printf("  [script] %s\n", s.c_str());
        return boxNil();
    });

    // native module: engine.{spawn, move, pos_x, entity_count}
    rt.nativeModule("engine", {
        {"spawn", [](const std::vector<Ref<Object>>& a, int line,
                     const BuiltinObject::CallFn&) -> Ref<Object> {
            std::string name = "entity";
            if (!a.empty()) asString(a[0], name);
            Entity* e = new Entity{g_nextId++};
            ++g_liveEntities;
            // OWNING handle: freeEntity runs on GC collect.
            return boxNative(e, ENTITY_TYPE, freeEntity, "Entity");
        }},
        {"move", [](const std::vector<Ref<Object>>& a, int line,
                    const BuiltinObject::CallFn&) -> Ref<Object> {
            if (a.size() != 3) return makeError("engine.move(handle, dx, dy)", line);
            Entity* e = static_cast<Entity*>(asNative(a[0], ENTITY_TYPE));
            if (!e) return makeError("engine.move: not an Entity handle", line);
            double dx = 0, dy = 0; asFloat(a[1], dx); asFloat(a[2], dy);
            e->x += dx; e->y += dy;
            return boxNil();
        }},
        {"pos_x", [](const std::vector<Ref<Object>>& a, int line,
                     const BuiltinObject::CallFn&) -> Ref<Object> {
            Entity* e = a.empty() ? nullptr
                                  : static_cast<Entity*>(asNative(a[0], ENTITY_TYPE));
            if (!e) return makeError("engine.pos_x: not an Entity handle", line);
            return box(e->x);
        }},
        {"entity_count", [](const std::vector<Ref<Object>>&, int,
                            const BuiltinObject::CallFn&) -> Ref<Object> {
            return box((long long)g_liveEntities);
        }},
    });

    std::printf("== Lovax embed demo ==\n");
    if (!rt.loadFile(scriptPath)) {
        std::printf("load failed: %s\n", rt.lastError()->inspect().c_str());
        return 1;
    }

    // Host keeps a script value across frames + GC: the player handle.
    Persistent player(rt.getGlobal("player"));

    // Fixed-step game loop: host drives the script.
    const int FRAMES = 5;
    for (int i = 0; i < FRAMES; ++i) {
        if (!rt.call("on_frame", { box(16.0) })) {
            std::printf("on_frame error: %s\n", rt.lastError()->inspect().c_str());
            return 1;
        }
    }

    // Read state back from the script (script -> host).
    auto frames = rt.call("get_frames");
    auto px = rt.call("player_x");
    long long fv = -1; double pxv = -1;
    if (frames) asInt(frames, fv);
    if (px) asFloat(px, pxv);
    std::printf("after %d frames: get_frames()=%lld, player_x()=%.0f\n",
                FRAMES, fv, pxv);

    // The transient `bullet` handles are unreachable; force a full GC and prove
    // their owning finalizers freed the C++ Entities. Only `player` (rooted via
    // the global AND the Persistent) must survive.
    gcCollect();
    std::printf("live entities after GC: %d (player survives; bullets freed)\n",
                g_liveEntities);

    // Persistent still valid across the GC:
    std::printf("persistent player handle valid: %s\n",
                player ? "yes" : "no");

    return (fv == FRAMES && g_liveEntities == 1 && (bool)player) ? 0 : 2;
}
