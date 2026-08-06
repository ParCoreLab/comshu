#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * TODO: Write explanation.
 * This library can only be linked against ONCE in the entire program of library
 *
 * TODO: Performance test
 */

namespace pcomm::smart_dlsym {
// Trying to avoid making downstream users having to #include <dlfcn.h>
extern void*     DL_RTLD_DEFAULT;
extern void*     DL_RTLD_NEXT;
extern const int DL_RTLD_NOW;
extern const int DL_RTLD_GLOBAL;
extern const int DL_RTLD_NOLOAD;

// (next_in_chain, handle, symbol_name) -> the function the caller should get.
//
// `next_in_chain` is what a call must be forwarded to in order to reach the real
// implementation: the real symbol for the first registrant, and the previous
// registrant's replacement for every one after. Returning nullptr means "no
// change" and leaves the chain as it was.
using CallbackType = std::function<void*(void*, void*, const char*)>;

// Defined in Dlsym.cpp, shared across all modules linking to libsmart_dlsym.a
extern void* (*real_dlopen)(const char*, int);
extern void* (*real_dlsym)(void*, const char*);
extern void* (*real_dlvsym)(void*, const char*, const char*);
extern void* (*real_dlclose)(void*);
extern std::unordered_map<std::string, std::vector<CallbackType>>* RegisteredSymbols;

// Register interest in a symbol. MULTIPLE registrants per symbol are supported
// and compose; this used to throw std::runtime_error on the second one.
//
// The throw made a stated design rule impossible to satisfy. Any provider may
// hook CUDA itself — a provider may need allocation facts for its own runtime
// decisions and cannot wait on another provider for them — but NVSHMEM reaches
// every CUDA entry point through a single dlsym of "cuGetProcAddress"
// (nvshmem-3.7.2 src/host/init/cudawrap.cpp:147, then :39-41). So every provider
// wanting CUDA interception must register that one symbol, the first to do so
// claimed it, and the second aborted the process at load — a passive observer
// killing the application it was meant to watch, which is the devx_probe
// failure in a new place.
//
// Registration order is innermost-first: the earliest registrant sits closest to
// the real symbol. Order must not matter for correctness. If it does, two
// registrants are fighting over one entry point, and that is a design error the
// chain should expose rather than paper over.
inline void register_symbol(std::string symbol_name, CallbackType callback) {
    (*RegisteredSymbols)[std::move(symbol_name)].push_back(std::move(callback));
}

// What the dlsym/dlvsym interposers hand back for `symbol_name`: the real
// symbol run through every registrant, innermost first.
//
// Declared here rather than left internal to Dlsym.cpp so that hook_smoke can
// exercise THIS function instead of a reimplementation of it. A test that walks
// the chain itself only proves the test agrees with itself; the ordering
// contract is exactly the kind of thing that would drift between the two.
void* on_symbol_load(void* real_symbol, void* handle, const char* symbol_name);
}  // namespace pcomm::smart_dlsym
