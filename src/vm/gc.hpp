#ifndef GC_HPP
#define GC_HPP

#include <vector>
#include <functional>
#include <cstddef>
#include <type_traits>
#include <new>

// Tracing mark-sweep garbage collector (RFC-013). This header is included from
// object.hpp *after* class Object is defined — it is not a standalone include.
//
// Design (clox-style, safe-by-construction):
//  - Every GC object lives in an intrusive singly-linked list (Object::gcNext).
//  - gcAlloc<T> checks the threshold and collects BEFORE allocating, so a freshly
//    made object is never swept by the collection its own allocation triggered.
//  - Roots: the runtime installs a callback (Heap::markRoots) that marks the value
//    stacks, globals, frames, upvalues and static caches of every live VM.
//  - Marking is virtual (Object::gcMark) so each type marks its own children; a
//    worklist (gray set) avoids deep recursion.
//  - GcRoot is an RAII temporary root for Objects held only in C++ locals across a
//    re-entry into the VM (the ~8 callback builtins). Push before, auto-pop after.
//  - LOVAX_GC_STRESS collects on every allocation: a missed root frees a live object
//    immediately, so ASan turns it into a use-after-free the test suite catches.

namespace Lovax {

class Object; // complete at include point (object.hpp)

// Hot-path flag: set by gcAlloc when a collection is due, checked by the inline
// safepoint below. A plain global bool so the safepoint is a single predicted
// branch (no Heap::get() magic-static call on the fib/loop hot path).
inline bool gcPending = false;

// Non-owning observing pointer to a GC-managed object. Presents the small slice
// of the shared_ptr API the codebase used (.get(), ->, *, bool, ==, nullptr,
// upcast) so replacing shared_ptr<Object> is nearly churn-free — but it owns
// nothing: the tracing GC manages lifetime. It is exactly one pointer wide, so a
// NaN-boxed Value (Phase 2) can pack it into a uint64.
template <class T>
struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(std::nullptr_t) {}
    Ref(T* raw) : p(raw) {}
    // Implicit UPCAST only (U* -> T*); constrained so the ternary/overload
    // resolver never considers an invalid downcast (keeps `cond ? ref : NULL_OBJ_`
    // unambiguous).
    template <class U, class = std::enable_if_t<std::is_convertible<U*, T*>::value>>
    Ref(const Ref<U>& o) : p(o.p) {}
    T* get() const { return p; }
    T* operator->() const { return p; }
    T& operator*() const { return *p; }
    explicit operator bool() const { return p != nullptr; }
    void reset() { p = nullptr; }
};
template <class T, class U> bool operator==(const Ref<T>& a, const Ref<U>& b) { return a.p == b.p; }
template <class T, class U> bool operator!=(const Ref<T>& a, const Ref<U>& b) { return a.p != b.p; }
template <class T> bool operator==(const Ref<T>& a, std::nullptr_t) { return a.p == nullptr; }
template <class T> bool operator!=(const Ref<T>& a, std::nullptr_t) { return a.p != nullptr; }
template <class T> bool operator==(std::nullptr_t, const Ref<T>& a) { return a.p == nullptr; }
template <class T> bool operator!=(std::nullptr_t, const Ref<T>& a) { return a.p != nullptr; }

// GC-allocate an object and wrap it (replaces std::make_shared<XObject>).
template <class T, class... A>
Ref<T> makeObj(A&&... args); // defined after gcAlloc below

// static_pointer_cast replacement (from a Ref or a raw Object*).
template <class T, class U>
Ref<T> refCast(const Ref<U>& r) { return Ref<T>(static_cast<T*>(r.p)); }
template <class T>
Ref<T> refCast(Object* p) { return Ref<T>(static_cast<T*>(p)); }

inline void gcMarkObject(Object* o); // fwd; defined in object.hpp once Object is complete
void gcCollect();                    // fwd; defined in object.hpp (full STW cycle)
void gcStep();                       // fwd; defined in object.hpp (incremental slice)

// Tri-color states (RFC-023). White = unvisited, gray = queued, black = scanned.
constexpr unsigned char GC_WHITE = 0, GC_GRAY = 1, GC_BLACK = 2;

// Collector phase. MARK and SWEEP proceed in bounded slices at safepoints.
enum class GcPhase : unsigned char { IDLE, MARK, SWEEP };

// Size-class free-list allocator (RFC-024 pre-work). Every VM object is small
// and fixed-shape, so malloc/free per object is pure overhead: a pool serves an
// allocation as one free-list pop and a free as one push. Objects larger than
// MAXSZ (none of ours today, but coroutines could) fall back to ::operator new.
//
// Freed slots are recycled, so ASan cannot see a use-after-free through the pool
// — the GC_STRESS builds keep plain new/delete (see gcAlloc / sweepStep) so that
// safety net stays intact; only release builds pool.
struct Pool {
    static constexpr size_t ALIGN = 16;
    static constexpr size_t MAXSZ = 256;                 // 16 classes: 16..256
    static constexpr int NCLASS = (int)(MAXSZ / ALIGN);
    static constexpr unsigned char BIG = 0xFF;           // oversized: not pooled
    void* freeList[NCLASS] = {};

    static int classOf(size_t n) { return (int)((n + ALIGN - 1) / ALIGN) - 1; }

    void* allocRaw(size_t n, unsigned char& scOut) {
        if (n == 0 || n > MAXSZ) { scOut = BIG; return ::operator new(n ? n : 1); }
        int c = classOf(n);
        scOut = (unsigned char)c;
        void* head = freeList[c];
        if (head == nullptr) { refill(c); head = freeList[c]; }
        freeList[c] = *reinterpret_cast<void**>(head);
        return head;
    }
    void freeRaw(void* p, unsigned char sc) {
        if (sc == BIG) { ::operator delete(p); return; }
        *reinterpret_cast<void**>(p) = freeList[sc];
        freeList[sc] = p;
    }
    // Carve one 64 KB arena block into slots and thread them onto the free list.
    // Arena blocks are never returned to the OS (like most poolers): the free
    // list caps live arena at the peak concurrent objects per class.
    void refill(int c) {
        size_t sz = (size_t)(c + 1) * ALIGN;
        size_t count = (64 * 1024) / sz;
        if (count < 32) count = 32;
        char* block = static_cast<char*>(::operator new(count * sz));
        for (size_t i = 0; i < count; ++i) {
            void* slot = block + i * sz;
            *reinterpret_cast<void**>(slot) = freeList[c];
            freeList[c] = slot;
        }
    }
};

// Collect/advance if due. Inline + cheap: the common case is one bool test.
// While a cycle is in flight, gcAlloc keeps gcPending set so every safepoint
// runs one more bounded slice until the cycle completes.
#ifdef LOVAX_GC_STRESS
inline void gcSafepoint() { if (gcPending) { gcPending = false; gcCollect(); } }
#else
inline void gcSafepoint() { if (gcPending) { gcPending = false; gcStep(); } }
#endif

struct Heap {
    Object* first = nullptr;            // head of the all-objects list
    size_t bytesAllocated = 0;          // LIVE bytes estimate (recomputed at sweep)
    size_t nextGC = 8 * 1024 * 1024;    // first collection after ~8 MB
    bool enabled = false;               // Phase 0.4: off until roots are wired (Phase 1)
    bool collecting = false;
    std::vector<Object*> worklist;      // gray set (to-scan)
    std::vector<Object*> tempRoots;     // GcRoot stack (C++-held temporaries)
    std::vector<Object*> permanentRoots;// immortal: singletons, builtin modules
    std::vector<Object*> hostRoots;     // embed: values the host holds across frames (RFC-025)
    std::function<void()> markRoots;    // installed by the runtime

    // Incremental state (RFC-023)
    GcPhase phase = GcPhase::IDLE;
    Object** sweepCursor = nullptr;     // resumable sweep position
    size_t sweepLive = 0;               // live bytes tallied by this sweep
    size_t allocSinceSweep = 0;         // allocations made while sweeping
    // Objects born while SWEEPING go on a side list the cursor never touches;
    // they splice back at cycle end, still white, so the NEXT cycle marks them
    // normally. (Allocating them black instead leaks stale black objects into
    // the next cycle whose children then get swept — a proven miscollection.)
    Object* newborn = nullptr;
    Object* newbornTail = nullptr;
#ifdef LOVAX_GC_STRESS_INC
    size_t stepBudget = 1;              // maximal interleaving: torture the barrier
#else
    size_t stepBudget = 3000;           // objects per slice (measured sub-ms)
#endif

    // --mem-stats counters (always maintained — they're a few adds; printed
    // by main only when the flag is given).
    size_t allocCount = 0;
    size_t collections = 0;
    size_t peakBytes = 0;
    long long gcNanos = 0;              // total time spent collecting
    long long maxPauseNanos = 0;        // worst single collection

    Pool pool;                          // size-class object allocator (release builds)

    static Heap& get() { static Heap h; return h; }
};

// Register an object that must never be collected (singletons, cached builtin
// modules). Marked on every cycle.
inline void gcPermanentRoot(Object* o) {
    if (o) Heap::get().permanentRoots.push_back(o);
}

// Allocate a GC-managed object. Collects BEFORE constructing so the new object
// always survives the triggered collection.
template <class T, class... A>
T* gcAlloc(A&&... args) {
    Heap& h = Heap::get();
    // Never collect mid-allocation: C++ code all over the VM/stdlib holds objects
    // in locals across allocations. Instead flag a request; the collector runs at
    // the next VM safepoint (loop back-edge / call), where the value stack is the
    // complete root set and no unrooted C++ temporary is live.
    if (h.enabled) {
#if defined(LOVAX_GC_STRESS) || defined(LOVAX_GC_STRESS_INC)
        gcPending = true;
#else
        // Trip when over threshold, and keep tripping while a cycle is in
        // flight so every safepoint advances it by one bounded slice.
        if (h.bytesAllocated > h.nextGC || h.phase != GcPhase::IDLE) gcPending = true;
#endif
    }
#if defined(LOVAX_GC_STRESS) || defined(LOVAX_GC_STRESS_INC)
    // Plain new/delete: a freed object is poisoned so ASan catches a missed root.
    T* obj = new T(std::forward<A>(args)...);
#else
    // Pool: placement-new into a recycled size-class slot. gcSizeClass records
    // the class so the sweep returns the raw memory to the matching free list.
    unsigned char sc;
    void* mem = h.pool.allocRaw(sizeof(T), sc);
    T* obj = new (mem) T(std::forward<A>(args)...);
    obj->gcSizeClass = sc;
#endif
    if (h.phase == GcPhase::SWEEP) {
        // Side list: invisible to the sweep cursor, spliced back at cycle end.
        obj->gcNext = h.newborn;
        h.newborn = obj;
        if (h.newbornTail == nullptr) h.newbornTail = obj;
        h.allocSinceSweep += sizeof(T);
    } else {
        obj->gcNext = h.first;
        h.first = obj;
    }
    // Count the payload the constructor produced (string bytes, vector storage),
    // not just the header — the threshold must see the real heap. Post-construction
    // growth is folded back in when the sweep recomputes live bytes.
    h.bytesAllocated += obj->gcBytes();
    h.allocCount++;
    if (h.bytesAllocated > h.peakBytes) h.peakBytes = h.bytesAllocated;
    return obj;
}

template <class T, class... A>
Ref<T> makeObj(A&&... args) {
    return Ref<T>(gcAlloc<T>(std::forward<A>(args)...));
}

// RAII temporary root: keeps a C++-held object alive across a VM re-entry.
struct GcRoot {
    explicit GcRoot(Object* o) { Heap::get().tempRoots.push_back(o); }
    ~GcRoot() { Heap::get().tempRoots.pop_back(); }
    GcRoot(const GcRoot&) = delete;
    GcRoot& operator=(const GcRoot&) = delete;
};

} // namespace Lovax

#endif // GC_HPP
