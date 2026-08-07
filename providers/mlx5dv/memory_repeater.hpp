#pragma once
// A man-in-the-middle on one 8-byte doorbell word.
//
// Ported from SigmaTrace/Profiler/Modules/MemoryHijack/MemoryRepeater.hpp. The
// UAR hijack hands the GPU a fake host page to ring; this parks a host thread on
// that page, and on every change runs a callback and then forwards the value to
// the real UAR. That is the whole data plane of a GPU-initiated send made
// visible to the CPU.
//
// Two properties are load-bearing and not stylistic:
//
//   * CALLBACK BEFORE FORWARD. Once the doorbell reaches the NIC, the NIC is
//     free to consume the WQEs and the GPU to recycle those slots. The callback
//     must read the send queue before the store forwards, or it races the
//     hardware for data that is already gone.
//   * FORWARD UNCONDITIONALLY. This sits in the application's data path. If the
//     forwarding store is ever skipped, reordered, or dropped, the application
//     hangs or corrupts a transfer — the failure lands in the workload, not in
//     the capture. The store is therefore outside every branch a callback fault
//     could take.
//
// The acquire load carries SigmaTrace's own unresolved caveat: "GPU-PCIe-NIC
// ordering is different from CPU". Redirecting a release store off a BAR page
// and replaying it from a different agent changes who guarantees the SQ writes
// are visible to the NIC before the doorbell is. Stage 1 forwards a pure copy
// and measures whether the application stays byte-identical; that measurement,
// not this comment, is what settles the ordering question.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace mlx5::repeater {

// Park until the word at `addr` may have changed. UMONITOR/UMWAIT on Intel
// (MN5's Sapphire Rapids and du04's host both have WAITPKG), MONITORX/MWAITX on
// AMD, else a PAUSE spin. Defined in memory_repeater.cpp because the wait
// instructions need per-function target attributes.
void wait_mem(void* addr) noexcept;

// True when wait_mem degraded to a PAUSE spin loop — no WAITPKG, no MWAITX. On
// that fallback each hijacked UAR is a busy core, and the ibgda plan requires
// that cost be stated in the capture rather than hidden.
bool wait_is_spin() noexcept;

class RepeaterHandle {
public:
    using DataType = std::uint64_t;
    using CallbackType = std::function<void(DataType new_val)>;

    // hijack_addr: the fake host page the GPU rings. real_addr: the true UAR
    // host mapping to forward to. on_change: run before each forward.
    RepeaterHandle(DataType* hijack_addr, void* real_addr, CallbackType on_change)
        : m_hijack_addr(hijack_addr),
          m_real_addr(real_addr),
          m_on_change(std::move(on_change)) {}

    RepeaterHandle(const RepeaterHandle&) = delete;
    RepeaterHandle& operator=(const RepeaterHandle&) = delete;
    RepeaterHandle(RepeaterHandle&&) = default;

    ~RepeaterHandle() {
        // A moved-from handle owns nothing; its pointers are null.
        if (m_deliver)
            signal_finish();
        if (m_alive)
            *m_alive = false;
        if (m_thread && m_thread->joinable())
            m_thread->join();
    }

    void start() noexcept {
        m_deliver = std::make_shared<std::atomic<bool>>(true);
        m_alive = std::make_shared<std::atomic<bool>>(true);

        // Never capture `this`: the handle is moved into a container after
        // start() and `this` is invalidated. Copy the pointers and the two
        // shared flags into the thread instead.
        m_thread = std::make_unique<std::jthread>(
            [hijack_addr = m_hijack_addr, real_addr = m_real_addr,
             on_change = std::move(m_on_change), alive = m_alive, deliver = m_deliver] {
                auto current = *hijack_addr;
                while (*alive) {
                    wait_mem(hijack_addr);

                    // The ordering caveat above lives on this load.
                    const DataType written = __atomic_load_n(hijack_addr, __ATOMIC_ACQUIRE);
                    if (written == current)
                        continue;  // spurious wakeup

                    // Record before forwarding, so the callback reads the SQ
                    // before the NIC can consume it. Deliver is a separate flag
                    // from alive: a consumer can stop the callbacks while the
                    // thread keeps forwarding the tail of stores that arrive
                    // after teardown begins.
                    if (*deliver && on_change)
                        on_change(written);

                    // Forward to the NIC. Unconditional: no branch a callback
                    // fault could take reaches this store.
                    __atomic_store_n(static_cast<std::uint64_t*>(real_addr), written,
                                     __ATOMIC_RELEASE);
                    current = written;
                }
            });
    }

    // Stop delivering callbacks; keep forwarding. Non-blocking, and safe to call
    // from the UAR-free hook without joining — joining there can deadlock
    // against a callback still inside a CUDA sync.
    void signal_finish() const noexcept {
        if (m_deliver)
            *m_deliver = false;
        if (m_thread)
            m_thread->request_stop();
    }

private:
    DataType*    m_hijack_addr{nullptr};
    void*        m_real_addr{nullptr};
    CallbackType m_on_change;

    // Two flags. `deliver` stops the callback; `alive` stops the thread. They
    // are distinct because, in SigmaTrace's testing, stores still arrive after a
    // consumer wants the callbacks to stop, and those stores must keep
    // forwarding or the application loses the last doorbell.
    std::shared_ptr<std::atomic<bool>> m_deliver{nullptr};
    std::shared_ptr<std::atomic<bool>> m_alive{nullptr};
    std::unique_ptr<std::jthread>      m_thread{nullptr};
};

}  // namespace mlx5::repeater
