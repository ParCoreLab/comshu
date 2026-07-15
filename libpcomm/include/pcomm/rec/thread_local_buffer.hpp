#pragma once
// L1 — per-thread, per-record-type homogeneous buffer. Write latency = fill a
// struct in place. Homogeneous streams are trivially columnar-convertible.
//
// Skeleton status: single buffer with synchronous submit on overflow. The
// production version double-buffers and hands full buffers to a flush thread
// (ARCHITECTURE §3); backpressure = drop-with-count, never silent wrap.

#include <array>
#include <cstddef>

#include "pcomm/core/record.hpp"
#include "pcomm/core/time.hpp"
#include "pcomm/rec/sink.hpp"

namespace pcomm::rec {

// Type-erased handle the Recorder's per-thread registry holds buffers by.
class IDrainable {
public:
    virtual ~IDrainable() = default;
    virtual void drain() = 0;
};

// What actually goes on the wire for Events/Entities: temporal header + POD.
// Artifacts/Edges typically carry TimeKind::None (still uniform per stream).
template <core::Record R>
struct Stamped {
    core::Temporal temporal;
    R              payload;
};

template <core::Record R, std::size_t Capacity = 4096>
class ThreadLocalBuffer final : public IDrainable {
public:
    explicit ThreadLocalBuffer(ISink& sink) : m_sink(&sink) {}
    ~ThreadLocalBuffer() override { drain(); }

    ThreadLocalBuffer(const ThreadLocalBuffer&) = delete;
    ThreadLocalBuffer& operator=(const ThreadLocalBuffer&) = delete;

    // Returns a slot to fill in place; commit() advances.
    R* slot(const core::Temporal& t) {
        if (m_count == Capacity) drain();
        m_slots[m_count].temporal = t;
        return &m_slots[m_count].payload;
    }

    void commit() { ++m_count; }

    void record(const core::Temporal& t, const R& r) {
        *slot(t) = r;
        commit();
    }

    // Hand the filled region to the sink. (Synchronous in the skeleton.)
    void drain() override {
        if (m_count == 0) return;
        m_sink->submit(BufferView{
            .type_id = core::RecordTrait<R>::type_id,
            .kind    = core::RecordTrait<R>::kind,
            .data    = reinterpret_cast<const std::byte*>(m_slots.data()),
            .bytes   = m_count * sizeof(Stamped<R>),
            .count   = m_count,
        });
        m_count = 0;
    }

    std::size_t pending() const { return m_count; }

private:
    ISink* m_sink;
    std::size_t m_count{0};
    std::array<Stamped<R>, Capacity> m_slots{};
};

}  // namespace pcomm::rec
