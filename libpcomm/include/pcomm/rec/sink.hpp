#pragma once
// L1 — ISink: where full buffers go. The ONE vtable on the write side, and it
// is invoked off the application thread (flush side), so the virtual call is
// off the hot path. Strategy pattern: FileSink for runs, NullSink for benches.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include "pcomm/core/ids.hpp"

namespace pcomm::rec {

// A view over one homogeneous run of serialized records of a single type.
struct BufferView {
    core::RecordTypeId type_id{0};
    core::RecordKind   kind{core::RecordKind::Event};
    const std::byte*   data{nullptr};
    std::size_t        bytes{0};
    std::size_t        count{0};
};

class ISink {
public:
    virtual ~ISink() = default;
    virtual void submit(const BufferView&) = 0;
    virtual void flush() = 0;
};

class NullSink final : public ISink {
public:
    void submit(const BufferView&) override {}
    void flush() override {}
};

// Per-record-type append-only stream files: <dir>/<type_id>.pcs
// (homogeneous streams are trivially columnar-convertible; UNIFICATION §5.1).
// Skeleton implementation: buffered stdio append. mmap/O_DIRECT comes later.
class FileSink final : public ISink {
public:
    explicit FileSink(std::filesystem::path dir) : m_dir(std::move(dir)) {
        std::filesystem::create_directories(m_dir);
    }

    ~FileSink() override {
        flush();
        for (auto& [id, f] : m_files) std::fclose(f);
    }

    // Serialized: buffers arrive from any recording thread's drain.
    void submit(const BufferView& view) override {
        std::lock_guard lock(m_mutex);
        std::FILE* f = stream_for(view.type_id);
        if (f && view.bytes > 0) std::fwrite(view.data, 1, view.bytes, f);
    }

    void flush() override {
        std::lock_guard lock(m_mutex);
        for (auto& [id, f] : m_files) std::fflush(f);
    }

    const std::filesystem::path& dir() const { return m_dir; }

private:
    std::FILE* stream_for(core::RecordTypeId id) {
        auto it = m_files.find(id);
        if (it != m_files.end()) return it->second;
        auto path = m_dir / (std::to_string(id) + ".pcs");
        std::FILE* f = std::fopen(path.c_str(), "ab");
        m_files.emplace(id, f);
        return f;
    }

    std::filesystem::path m_dir;
    std::mutex m_mutex;
    std::unordered_map<core::RecordTypeId, std::FILE*> m_files;
};

}  // namespace pcomm::rec
