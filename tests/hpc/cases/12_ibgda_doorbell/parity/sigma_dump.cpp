// sigma_dump — dump SigmaTrace's mlx5dv/doorbell events as TSV.
//
// Written for the pcomm parity comparison
// (pcomm/plans/PLAN-20260810-0102-sigmatrace-parity). The comparison is
// TSV-vs-TSV so that neither tool's analysis code sits in the loop: SigmaTrace's
// own ProfilerEventReader produces a comm-graph JSON and a Perfetto trace, both
// derived and opinionated views, and a difference in either would not
// distinguish "missed an event" from "aggregated it differently".
//
// Emits one row per event, timing-free by construction — the fields printed are
// the ones that survive canonicalisation: queue-pair number, producer index,
// opcode, and length. Timestamps are deliberately not printed.
//
//   sigma_dump <output-dir>        # the -o directory given to Profiler
//
// NOTE: the visit() signature here follows Events/EventReader.hpp, NOT
// Events/Tests/reader_visitor.cpp — that example is stale (it uses an EventTag
// type and a 3-argument form that no longer exist).
//
// Build (see the plan for the full toolchain):
//   clang++-20 -std=c++20 -I Events -I Profiler -I Profiler/Events \
//              Tools/sigma_dump.cpp -o sigma_dump
#include <cstdio>
#include <filesystem>
#include <string>

// <map> before EventReader.hpp: that header declares std::map without including
// it, and happens to compile inside the tree only because something upstream of
// it in the normal build pulls <map> in first. Included here rather than patched
// there so this tool builds against an unmodified checkout.
#include <map>

#include "EventReader.hpp"
#include "EventRecorderConfig.hpp"
#include "EventTrait.hpp"

#include "Mlx5DvEvents.hpp"

namespace PE = Profiler::Events;

// Only the mlx5dv family: this comparison is about the DEVX/IBGDA path, and an
// event without a visit() is skipped by the reader.
using EnabledEvents = Events::TypeList<PE::DoorbellRingData, PE::DoorbellRingControl,
                                       PE::Mlx5dvMemoryRegistration, PE::Mlx5dvMemoryDereg,
                                       PE::Mlx5dvUarAlloc, PE::Mlx5dvUarFree>;

class Dump : public Events::EventReaderBase<Dump, EnabledEvents> {
    using Base = EventReaderBase<Dump, EnabledEvents>;

public:
    using Base::Base;

    // Required by EventReaderBase for every non-Meta event: it is the hook where
    // a reader would convert TSC to wall-clock. This comparison is timing-free
    // by design, so there is nothing to convert and it stays a no-op.
    template<typename... Args>
    void preprocess(Args&&...) {}

    // kind, qp_num, prod_index, opcode, length, gpu_id — the doorbell rows are
    // what the parity claim is built on. `data` and `control` are kept as
    // separate kinds rather than merged: SigmaTrace splits them and pcomm does
    // not, and flattening that here would hide a real difference in vocabulary.
    void visit(Events::EventTrait<PE::DoorbellRingData>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::DoorbellRingData&& e) {
        std::printf("doorbell_data\t%u\t%u\t0x%02x\t%llu\t%d\n", e.qp_num, e.prod_index,
                    static_cast<unsigned>(e.opcode),
                    static_cast<unsigned long long>(e.bytes_sent), static_cast<int>(e.gpu_id));
    }

    void visit(Events::EventTrait<PE::DoorbellRingControl>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::DoorbellRingControl&& e) {
        std::printf("doorbell_control\t%u\t%u\t0x%02x\t0\t%d\n", e.qp_num, e.prod_index,
                    static_cast<unsigned>(e.opcode), static_cast<int>(e.gpu_id));
    }

    // Registrations: size is stable across runs, the address is not, so only the
    // size is printed. Same projection the pcomm side applies.
    void visit(Events::EventTrait<PE::Mlx5dvMemoryRegistration>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::Mlx5dvMemoryRegistration&& e) {
        std::printf("umem_reg\t%u\t0\t0x00\t%llu\t-1\n", e.mem_id,
                    static_cast<unsigned long long>(e.size));
    }

    void visit(Events::EventTrait<PE::Mlx5dvMemoryDereg>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::Mlx5dvMemoryDereg&& e) {
        std::printf("umem_dereg\t%u\t0\t0x00\t0\t-1\n", e.mem_id);
    }

    void visit(Events::EventTrait<PE::Mlx5dvUarAlloc>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::Mlx5dvUarAlloc&& e) {
        std::printf("uar_alloc\t%u\t0\t0x00\t0\t-1\n", e.page_id);
    }

    void visit(Events::EventTrait<PE::Mlx5dvUarFree>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::Mlx5dvUarFree&&) {
        std::printf("uar_free\t0\t0\t0x00\t0\t-1\n");
    }
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage: sigma_dump <profiler-output-dir>\n", stderr);
        return 1;
    }
    Events::RuntimeConfig config{};
    config.OutputDirectory = argv[1];
    config.EventsDirectory = config.OutputDirectory / "events";
    config.MetaEventsDirectory = config.OutputDirectory / "meta";

    std::printf("kind\tid\tprod_index\topcode\tbytes\tgpu_id\n");
    Dump reader{config};
    const std::size_t total = reader.process();
    std::fprintf(stderr, "sigma_dump: %zu events from %s\n", total,
                 config.EventsDirectory.c_str());
    return 0;
}
