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
#include "pcomm/rec/stream_header.hpp"

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

// Per-record-type append-only stream files: <dir>/<type_id>.pcs, each opening
// with a 32-byte StreamHeader (homogeneous streams are trivially
// columnar-convertible; UNIFICATION §5.1).
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
        // An empty view carries no stride, so it cannot produce a valid header.
        // Creating the file anyway would leave a stream framed as stride 0 that
        // no reader can use — an empty capture is better as no file at all.
        if (view.bytes == 0 || view.count == 0) return;
        std::lock_guard lock(m_mutex);
        std::FILE* f = stream_for(view);
        if (f) std::fwrite(view.data, 1, view.bytes, f);
    }

    void flush() override {
        std::lock_guard lock(m_mutex);
        for (auto& [id, f] : m_files) std::fflush(f);
    }

    const std::filesystem::path& dir() const { return m_dir; }

private:
    // The stride comes from the buffer rather than from sizeof: this sink is
    // type-erased by the time a view reaches it, and a view is by construction
    // a run of one record type, so bytes/count *is* sizeof(Stamped<R>).
    std::FILE* stream_for(const BufferView& view) {
        auto it = m_files.find(view.type_id);
        if (it != m_files.end()) return it->second;

        auto path = m_dir / (std::to_string(view.type_id) + ".pcs");
        // Append mode, and the header goes on only when we are the ones
        // creating the file. A rank that reopens an existing capture directory
        // appends records to a stream that is already framed; writing a second
        // header mid-file would be indistinguishable from a record.
        const bool fresh = !std::filesystem::exists(path) ||
                           std::filesystem::file_size(path) == 0;
        std::FILE* f = std::fopen(path.c_str(), "ab");
        if (f && fresh) {
            const auto stride = view.count ? static_cast<std::uint32_t>(view.bytes / view.count) : 0u;
            StreamHeader h(view.type_id, view.kind, stride);
            std::fwrite(&h, sizeof h, 1, f);
        }
        m_files.emplace(view.type_id, f);
        return f;
    }

    std::filesystem::path m_dir;
    std::mutex m_mutex;
    std::unordered_map<core::RecordTypeId, std::FILE*> m_files;
};

}  // namespace pcomm::rec
