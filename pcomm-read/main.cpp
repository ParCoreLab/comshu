// pcomm-read — post-processor. Target pipeline: raw per-type streams + intern
// sidecars -> TimeAligner -> columnar Tables -> exporters (Parquet canonical,
// Perfetto derived). Heavy deps (Arrow) are allowed here — off the hot path.
//
// Skeleton: inventories a trace directory and decodes the known phase-1
// record types to CSV on stdout (`--dump <type_id>`), enough to inspect and
// validate captures today. The trait-driven generic decoder replaces the
// per-type functions here when the Parquet writer lands.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "pcomm/core/records.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"

namespace fs = std::filesystem;
using namespace pcomm;
using rec::Stamped;

namespace {

template <core::Record R>
std::vector<Stamped<R>> load_stream(const fs::path& file) {
    std::vector<Stamped<R>> out;
    std::ifstream in(file, std::ios::binary);
    if (!in) return out;
    auto bytes = fs::file_size(file);
    out.resize(bytes / sizeof(Stamped<R>));
    in.read(reinterpret_cast<char*>(out.data()),
            static_cast<std::streamsize>(out.size() * sizeof(Stamped<R>)));
    return out;
}

const char* memop_name(core::MemOp op) {
    switch (op) {
        case core::MemOp::Load: return "load";
        case core::MemOp::Store: return "store";
        case core::MemOp::Atomic: return "atomic";
        case core::MemOp::Prefetch: return "prefetch";
    }
    return "?";
}

void dump(core::RecordTypeId type, const fs::path& file) {
    using namespace core::records;
    switch (type) {
        case core::RecordTrait<GpuAccess>::type_id: {
            std::puts("ticks,pc,object,addr,size,op,run_dev,mem_dev,thread");
            for (const auto& r : load_stream<GpuAccess>(file)) {
                std::printf("%llu,%u,%u,0x%llx,%u,%s,%u,%u,%u\n",
                            (unsigned long long)r.temporal.a.ticks, r.payload.pc,
                            r.payload.object, (unsigned long long)r.payload.address,
                            r.payload.size, memop_name(r.payload.op),
                            r.payload.run_device, r.payload.mem_device,
                            r.payload.thread);
            }
            break;
        }
        case core::RecordTrait<MemObject>::type_id: {
            std::puts("ticks,id,name,base,size,device");
            for (const auto& r : load_stream<MemObject>(file)) {
                std::printf("%llu,%u,%u,0x%llx,%llu,%u\n",
                            (unsigned long long)r.temporal.a.ticks, r.payload.id,
                            r.payload.name, (unsigned long long)r.payload.range.base,
                            (unsigned long long)r.payload.range.size, r.payload.device);
            }
            break;
        }
        case core::RecordTrait<SourceLine>::type_id: {
            std::puts("pc,file,line");
            for (const auto& r : load_stream<SourceLine>(file)) {
                std::printf("%u,%u,%u\n", r.payload.pc, r.payload.file,
                            r.payload.line);
            }
            break;
        }
        case core::RecordTrait<ClockAnchor>::type_id: {
            std::puts("domain,local_ticks,wall_ns,mono_ns");
            for (const auto& r : load_stream<ClockAnchor>(file)) {
                std::printf("%u,%llu,%llu,%llu\n", r.payload.domain,
                            (unsigned long long)r.payload.local_ticks,
                            (unsigned long long)r.payload.wall_ns,
                            (unsigned long long)r.payload.mono_ns);
            }
            break;
        }
        default:
            std::fprintf(stderr, "pcomm-read: no decoder for type %u yet\n", type);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs(
            "usage: pcomm-read <trace-dir>                 stream inventory\n"
            "       pcomm-read <trace-dir> --dump <typeid>  decode one stream to CSV\n",
            stderr);
        return 1;
    }

    fs::path dir = argv[1];
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "pcomm-read: not a directory: %s\n", dir.c_str());
        return 1;
    }

    if (argc >= 4 && std::strcmp(argv[2], "--dump") == 0) {
        auto type = static_cast<core::RecordTypeId>(std::atoi(argv[3]));
        bool found = false;
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file() &&
                entry.path().filename() == std::to_string(type) + ".pcs") {
                dump(type, entry.path());
                found = true;
            }
        }
        if (!found) {
            std::fprintf(stderr, "pcomm-read: no %u.pcs under %s\n", type, dir.c_str());
            return 1;
        }
        return 0;
    }

    std::printf("streams in %s:\n", dir.c_str());
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".pcs") {
            std::printf("  %-40s %10ju bytes\n",
                        fs::relative(entry.path(), dir).c_str(),
                        static_cast<std::uintmax_t>(entry.file_size()));
        }
    }
    return 0;
}
