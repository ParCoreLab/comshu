#pragma once
// L1 — ISink: where full buffers go. The ONE vtable on the write side, and it
// is invoked off the application thread (flush side), so the virtual call is
// off the hot path. Strategy pattern: FileSink for runs, NullSink for benches.
//
// EVERY WRITE IS CHECKED, and that is not defensive style — it is the fix for a
// defect that cost a red board and an hour of misdirected diagnosis. See
// plans/PLAN-20260903-0100. The short version: this file used to ignore the
// return value of every fwrite, including the one that writes the StreamHeader.
// On MN5 with the home quota exhausted, that produced captures where a stream
// file held 288 bytes of payload with no 32-byte header in front of it, another
// held 0 bytes, and the process exited 0. Three suite cases went red pointing at
// InfiniBand and UCX behaviour; the actual fault was a filesystem quota.
//
// A tracer that loses data quietly is worse than one that refuses to run.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "comshu/core/ids.hpp"
#include "comshu/rec/stream_header.hpp"

namespace comshu::rec {

// A view over one homogeneous run of serialized records of a single type.
struct BufferView {
    core::RecordTypeId type_id{0};
    core::RecordKind   kind{core::RecordKind::Event};
    const std::byte*   data{nullptr};
    std::size_t        bytes{0};
    std::size_t        count{0};
};

// What a sink lost, per stream. Reported rather than returned, because losing
// data is not an error the CALLER can act on — the record is already gone by
// the time anyone could retry — but it is one the RUN must not conclude without
// mentioning.
struct StreamLoss {
    core::RecordTypeId type_id{0};
    std::uint64_t      lost_records{0};
    std::uint64_t      lost_bytes{0};
    bool               header_lost{false};  // the stream is unreadable, not merely short
    bool               never_opened{false}; // fopen failed; nothing of this type survives
};

class ISink {
public:
    virtual ~ISink() = default;
    virtual void submit(const BufferView&) = 0;
    virtual void flush() = 0;
    // Empty when every byte submitted reached the file.
    virtual std::vector<StreamLoss> losses() const { return {}; }
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
        std::error_code ec;
        std::filesystem::create_directories(m_dir, ec);
        m_dir_failed = static_cast<bool>(ec);
    }

    ~FileSink() override {
        flush();
        for (auto& [id, s] : m_streams) {
            // fclose can fail — it flushes — and on a full filesystem it is the
            // LAST place the loss can still be noticed. Nothing downstream reads
            // m_streams after this, so the count is recorded for a caller that
            // asks losses() before destruction; a caller that does not is
            // choosing not to know, which is its right and not this file's
            // problem to solve twice.
            if (s.f && std::fclose(s.f) != 0)
                s.failed = true;
            s.f = nullptr;
        }
    }

    // Serialized: buffers arrive from any recording thread's drain.
    void submit(const BufferView& view) override {
        // An empty view carries no stride, so it cannot produce a valid header.
        // Creating the file anyway would leave a stream framed as stride 0 that
        // no reader can use — an empty capture is better as no file at all.
        if (view.bytes == 0 || view.count == 0)
            return;
        std::lock_guard lock(m_mutex);
        Stream&         s = stream_for(view);
        if (!s.f) {
            // fopen failed, or its header did. Either way this type has no
            // usable file, and every record of it is lost — counted, not
            // silently dropped, which is the whole point of this rewrite.
            s.lost_records += view.count;
            s.lost_bytes += view.bytes;
            return;
        }
        // fwrite with size 1 returns the BYTE count, so a short write is
        // visible. It was not checked here, and that is defect #1.
        const std::size_t wrote = std::fwrite(view.data, 1, view.bytes, s.f);
        if (wrote != view.bytes) {
            s.failed = true;
            s.lost_bytes += view.bytes - wrote;
            // Bytes, not records, is what is actually known: a short write can
            // land mid-record. The record count is derived so a reader has a
            // magnitude, and it rounds UP because a partially written record is
            // a lost record.
            const std::size_t stride = view.bytes / view.count;
            s.lost_records += stride ? (view.bytes - wrote + stride - 1) / stride : view.count;
        }
    }

    void flush() override {
        std::lock_guard lock(m_mutex);
        for (auto& [id, s] : m_streams)
            // Buffered stdio means a quota failure usually surfaces HERE rather
            // than at fwrite, which is why ignoring this return was as bad as
            // ignoring the other two.
            if (s.f && std::fflush(s.f) != 0)
                s.failed = true;
    }

    std::vector<StreamLoss> losses() const override {
        std::lock_guard         lock(m_mutex);
        std::vector<StreamLoss> out;
        for (const auto& [id, s] : m_streams)
            if (s.failed || s.lost_records || s.header_lost || !s.f)
                out.push_back({id, s.lost_records, s.lost_bytes, s.header_lost, s.never_opened});
        return out;
    }

    // How many streams this sink framed and still holds open. Not derivable from
    // losses(): that reports only the damaged ones, so a capture with no losses
    // returns an empty vector whether it wrote eleven streams or none. The
    // attestation record needs the denominator, not just the numerator.
    std::size_t open_streams() const {
        std::lock_guard lock(m_mutex);
        std::size_t     n = 0;
        for (const auto& [id, s] : m_streams)
            if (s.f)
                n++;
        return n;
    }

    // The output directory could not be created at all — every stream is lost
    // and there is nothing to enumerate.
    bool directory_failed() const { return m_dir_failed; }

    const std::filesystem::path& dir() const { return m_dir; }

private:
    struct Stream {
        std::FILE*    f{nullptr};
        bool          failed{false};
        bool          header_lost{false};
        bool          never_opened{false};
        std::uint64_t lost_records{0};
        std::uint64_t lost_bytes{0};
    };

    // The stride comes from the buffer rather than from sizeof: this sink is
    // type-erased by the time a view reaches it, and a view is by construction
    // a run of one record type, so bytes/count *is* sizeof(Stamped<R>).
    Stream& stream_for(const BufferView& view) {
        auto it = m_streams.find(view.type_id);
        if (it != m_streams.end())
            return it->second;

        Stream     s;
        auto       path = m_dir / (std::to_string(view.type_id) + ".pcs");
        // Append mode, and the header goes on only when we are the ones
        // creating the file. A rank that reopens an existing capture directory
        // appends records to a stream that is already framed; writing a second
        // header mid-file would be indistinguishable from a record.
        std::error_code ec;
        const auto      existing = std::filesystem::file_size(path, ec);
        const bool      fresh = static_cast<bool>(ec) || existing == 0;
        std::FILE*      f = std::fopen(path.c_str(), "ab");
        if (!f) {
            // Previously this stored a null FILE* in the map, which made every
            // later submit() of this type a silent no-op AND made the destructor
            // call fclose(nullptr) — undefined behaviour on a path that only
            // runs when something has already gone wrong.
            s.never_opened = true;
            s.failed = true;
            return m_streams.emplace(view.type_id, s).first->second;
        }
        if (fresh) {
            const auto stride =
                view.count ? static_cast<std::uint32_t>(view.bytes / view.count) : 0u;
            StreamHeader h(view.type_id, view.kind, stride);
            // Defect #2, and the expensive one: an unchecked header write leaves
            // a file that later receives payload, so it is not a truncated tail
            // but a stream with no head. `comshu read` calls that
            // "pre-format-1 capture", which sends the next reader looking for a
            // version problem that does not exist.
            if (std::fwrite(&h, sizeof h, 1, f) != 1) {
                s.failed = true;
                s.header_lost = true;
                // Close it rather than write records into an unframed file. A
                // zero-length file is honestly empty; a headerless one with
                // records in it is a trap.
                std::fclose(f);
                f = nullptr;
            }
        }
        s.f = f;
        return m_streams.emplace(view.type_id, s).first->second;
    }

    std::filesystem::path                          m_dir;
    mutable std::mutex                             m_mutex;
    std::unordered_map<core::RecordTypeId, Stream> m_streams;
    bool                                           m_dir_failed{false};
};

}  // namespace comshu::rec
