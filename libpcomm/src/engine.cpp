// The engine's one and only definition site.
//
// Everything here used to be a function-local static inside a header, which
// meant every provider .so carried its own copy with weak (vague) linkage. The
// dynamic linker then bound all references to whichever copy loaded first, so
// two providers shared a Tracer *by accident* — and destroyed each other's
// Recorder on the way up (see commit 7a5b55a, things-to-circle-back-to.md).
//
// Defining them in exactly one shared object makes that sharing deliberate:
// one Tracer, one buffer registry, one id tracker, one set of intern tables,
// no matter how many providers are loaded. The headers now only *declare*
// these, so a provider physically cannot emit a second copy.
//
// INVARIANT: nothing may be added here that pulls in a vendor header — this is
// still L0-L2 core (see ARCHITECTURE.md §0).

#include <dlfcn.h>

#include <string>

#include "pcomm/core/record.hpp"
#include "pcomm/rec/recorder.hpp"
#include "pcomm/rec/thread_buffer_registry.hpp"
#include "pcomm/tracer.hpp"

namespace pcomm {

// All three are deliberately leaked (never destroyed): backends flush from
// exit-time vendor callbacks (NVBit's atexit, CUPTI's) that can run *after*
// C++ static destructors, so a destroyed singleton would be a use-after-free
// on the way out. The leak is process-exit-only.

Tracer& Tracer::instance() {
    static Tracer* t = new Tracer();
    return *t;
}

namespace rec {

ThreadBufferRegistry& ThreadBufferRegistry::instance() {
    static ThreadBufferRegistry* r = new ThreadBufferRegistry();
    return *r;
}

// Emitting the default Recorder here rather than in every provider is what
// gives the process a single buffer_store() and a single set of intern tables.
// Member *templates* (record<R>/buffer<R>) are deliberately left to instantiate
// per provider — they are the hot path, and each provider owns a disjoint slice
// of the RecordTypeId space, so those instantiations don't collide.
template class Recorder<DefaultConfig>;

}  // namespace rec

namespace core {

RecordIdTracker& RecordIdTracker::instance() {
    static RecordIdTracker* t = new RecordIdTracker();
    return *t;
}

}  // namespace core
}  // namespace pcomm

// Where this .so lives on disk, so the launcher can put it in LD_PRELOAD.
//
// It MUST be preloaded, not merely linked: the dynamic linker searches
// preloaded objects before the executable's own dependencies, and `dlsym` lives
// here. As a plain DT_NEEDED of a preloaded provider the engine loses the
// `dlsym` lookup to libc, our interposer never runs, and every symbol a target
// resolves at runtime (NVSHMEM resolves its verbs entry points that way) is
// captured only if it also happens to go through a PLT we interpose. That cost
// 12 of 16 memory regions and 36 of 432 queue pairs before it was caught.
//
// Self-reporting via dladdr rather than making the launcher guess a path keeps
// this correct across build layouts (build/ vs build-gpumem/, installed, or a
// provider .so copied elsewhere).
extern "C" const char* pcomm_engine_so_path() {
    static const std::string path = [] {
        Dl_info info{};
        if (dladdr(reinterpret_cast<const void*>(&pcomm_engine_so_path), &info) &&
            info.dli_fname)
            return std::string(info.dli_fname);
        return std::string();
    }();
    return path.empty() ? nullptr : path.c_str();
}
