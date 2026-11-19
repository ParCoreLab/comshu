#pragma once
// One provider's claim on the engine: acquired at most once, released at most
// once, and only if it was acquired (CR-57, CR-58).
//
// The engine is refcounted — the last provider to release finalizes the
// capture — so the count has to be exact. It was not: every provider released
// from BOTH its launcher shutdown() and its .so destructor, and several released
// after declare_provider() had refused them and they never acquired. One extra
// release finalizes the capture while another provider is still recording into
// it; a missing one means no finalize at all.
//
// Internal linkage on purpose: each provider is its own .so and must have its
// own hold. A vague-linkage inline here could be unified by the dynamic linker
// across providers, which is exactly the shared-state accident the providers'
// export maps exist to prevent. Acquire and release live in one translation
// unit per provider, so one hold per TU is one per provider.

#include <atomic>

#include "comshu/engine_abi.h"

namespace comshu::provider {
namespace {

class EngineHold {
public:
    void acquire(const char* output_dir, int rank, int verbose) {
        if (m_held.exchange(true, std::memory_order_acq_rel))
            return;
        comshu_engine_acquire(output_dir, rank, verbose);
    }
    void release() {
        if (m_held.exchange(false, std::memory_order_acq_rel))
            comshu_engine_release();
    }
    bool held() const { return m_held.load(std::memory_order_acquire); }

private:
    std::atomic<bool> m_held{false};
};

[[maybe_unused]] EngineHold& engine_hold() {
    static EngineHold h;  // trivially destructible state: safe in exit handlers
    return h;
}

}  // namespace
}  // namespace comshu::provider
