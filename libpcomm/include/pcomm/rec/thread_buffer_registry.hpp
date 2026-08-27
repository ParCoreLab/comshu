#pragma once
// L1 — process-global registry of every ThreadLocalBuffer created by any
// thread, so a single flush (Tracer::stop, on the exiting thread) drains all
// threads' buffers. Successor to the per-thread-only drain_thread(): before
// this, buffers filled by threads other than the one calling stop() were lost.
//
// Adapted from SigmaTrace's ThreadBufferRegistry (Events/ThreadLocalBuffer.hpp).
// pcomm's per-thread buffer stores are already leaked (raw thread_local
// pointer, no TLS destructor), so the buffers outlive every thread and stay
// valid until _exit — we can track plain IDrainable* instead of the shared_ptr
// finalizers SigmaTrace needs.
//
// NOTE(fork): not yet fork-aware. A target that fork()s after buffers exist
// would inherit parent buffers still pointing at the parent's rank files
// (shared fd). SigmaTrace guards this with a fork generation + graveyard +
// pthread_atfork; port that when a forking launcher needs it.

#include <mutex>
#include <new>
#include <vector>

#include "pcomm/rec/thread_local_buffer.hpp"  // IDrainable

namespace pcomm::rec {

class ThreadBufferRegistry {
public:
    // Leaked (never destroyed): backends flush from exit-time callbacks that
    // can run after static destructors, same as the Recorder/Tracer.
    static ThreadBufferRegistry& instance();  // src/engine.cpp — one per process

    // Called once per (thread, record-type) buffer, when the Recorder first
    // creates it on a thread.
    void register_buffer(IDrainable* buf) {
        std::lock_guard lock(m_mutex);
        m_buffers.push_back(buf);
    }

    // Drain every registered buffer. Called once at stop() on the flushing
    // thread; recording threads are expected quiescent by then. drain() is
    // idempotent (no-op when empty), so a buffer already drained by its owning
    // thread (drain_thread) is harmless to drain again. The sink serializes
    // writes, so concurrent drains from different buffers are safe.
    void drain_all() {
        std::lock_guard lock(m_mutex);
        for (IDrainable* b : m_buffers)
            b->drain();
    }

    // The same thing for a process that is already dying, where WAITING is the
    // one thing we must not do. In a fatal-signal handler the thread holding
    // this lock may be the thread that just faulted, and it will never release
    // it — so a blocking drain_all() there turns a crash into a hang, which in a
    // batch scheduler burns the rest of the walltime allocation.
    //
    // Returns false when the lock was unavailable, so the caller can report that
    // it recovered nothing rather than claim a flush it did not perform. Same
    // family of reasoning as reset_for_fork() below: this class already has to
    // think about its mutex being in a state ordinary code never sees.
    bool try_drain_all() noexcept {
        std::unique_lock lock(m_mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return false;
        for (IDrainable* b : m_buffers)
            b->drain();
        return true;
    }

    std::size_t size() {
        std::lock_guard lock(m_mutex);
        return m_buffers.size();
    }

    // Child-side of a fork: forget every tracked buffer WITHOUT draining. Those
    // buffers hold the parent's records and, via COW, point at the parent's
    // sink/files — flushing them from the child would clobber the parent's
    // output. The parent flushes its own copies. The mutex is placement-reset
    // because fork() may have copied it locked (another thread held it), which
    // would deadlock the child. Called only from the pthread_atfork child
    // handler, where the child is single-threaded.
    void reset_for_fork() noexcept {
        new (&m_mutex) std::mutex();  // reset to unlocked; parent's copy untouched
        m_buffers.clear();
    }

private:
    std::mutex               m_mutex;
    std::vector<IDrainable*> m_buffers;
};

}  // namespace pcomm::rec
