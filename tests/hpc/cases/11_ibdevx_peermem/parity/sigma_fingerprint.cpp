// sigma_fingerprint — SigmaTrace's verbs and mlx5dv events as a canonical,
// timing-free fingerprint, for the case 11 (ibdevx) parity comparison.
//
// Case 12's parity/sigma_dump.cpp covers the mlx5dv family only, because that
// comparison was about the IBGDA doorbell. Under ibdevx the CPU rings the
// doorbell, and NVSHMEM on du04 builds its queue pairs through DEVX, so what the
// two tools can both see is the control plane: devices, ports, memory regions,
// umems, UARs, connections, and the closes that end them. This prints all of
// it, plus the data-plane classes, so that an empty one is visible as empty.
//
// One line per event, `class<TAB>key`. The key keeps only what survives a rerun
// — sizes, flags, LIDs, port state, GUIDs — and drops what does not: addresses,
// qp numbers, mem ids, page ids, timestamps. pcomm's side is projected onto the
// same classes and keys by parity.py, so the comparison is between two bags of
// identical shape and neither tool's own analysis code sits in the loop.
//
//   sigma_fingerprint <profiler-output-dir>      # one rank's -o directory
//
// Build against an unmodified SigmaTrace checkout, from its root:
//   clang++-20 -std=c++20 -O2 -I Events -I Profiler -I Profiler/Events \
//     -I Modules/Helpers/include <this file> -o sigma_fingerprint
#include <endian.h>

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>

// <map> before EventReader.hpp, which uses std::map without including it — the
// same workaround as sigma_dump.cpp, so this builds against an unmodified tree.
#include <map>

#include "EventReader.hpp"
#include "EventRecorderConfig.hpp"
#include "EventTrait.hpp"

#include "IbvEvents.hpp"
#include "Mlx5DvEvents.hpp"

namespace PE = Profiler::Events;

using EnabledEvents =
    Events::TypeList<PE::IbvDeviceRegistration, PE::IbvPortRegistration, PE::IbvOpenDevice,
                     PE::IbvCloseDevice, PE::IbvCreateQp, PE::IbvDestroyQp, PE::IbvModifyQp,
                     PE::QpConnectionInfo, PE::IbvMemoryReg, PE::IbvMemoryDereg, PE::IbvPostSend,
                     PE::IbvPostRecv, PE::IbvCompletion, PE::Mlx5dvMemoryRegistration,
                     PE::Mlx5dvMemoryDereg, PE::Mlx5dvUarAlloc, PE::Mlx5dvUarFree,
                     PE::DoorbellRingData, PE::DoorbellRingControl>;

class Fingerprint : public Events::EventReaderBase<Fingerprint, EnabledEvents> {
    using Base = EventReaderBase<Fingerprint, EnabledEvents>;

    // mem_id -> size, so a umem dereg can carry the size its registration had.
    // SigmaTrace's dereg event records only the id, and ids are recycled, so the
    // entry is dropped on dereg rather than kept.
    std::unordered_map<std::uint32_t, std::uint64_t> m_umem_size;

    template<typename... Args> static void row(const char* cls, const char* fmt, Args... args) {
        std::printf("%s\t", cls);
        std::printf(fmt, args...);
        std::printf("\n");
    }

public:
    using Base::Base;

    // Required by EventReaderBase for every non-Meta event; timing-free by
    // design, so there is nothing to convert.
    template<typename... Args> void preprocess(Args&&...) {}

    // ---- devices and ports ---------------------------------------------------
    // SigmaTrace stores node_guid as verbs returns it, a __be64; pcomm stores it
    // in host order, as ibv_devinfo prints it. Measured on du04: the raw value
    // 0x6a9fd80003f6ceb8 is 0xb8cef60300d89f6a byte-reversed — one NIC, two
    // spellings — so it is converted here rather than reported as a disagreement.
    void visit(Events::EventTrait<PE::IbvDeviceRegistration>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::IbvDeviceRegistration&& e) {
        row("device_enum", "guid=0x%016" PRIx64, be64toh(e.node_guid));
    }
    void visit(Events::EventTrait<PE::IbvPortRegistration>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::IbvPortRegistration&& e) {
        row("port", "port=%u lid=%u state=%d", unsigned{e.port_num}, unsigned{e.lid},
            static_cast<int>(e.state));
    }
    void visit(Events::EventTrait<PE::IbvOpenDevice>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::IbvOpenDevice&& e) {
        row("device_open", "guid=0x%016" PRIx64, be64toh(e.node_guid));
    }
    void visit(Events::EventTrait<PE::IbvCloseDevice>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::IbvCloseDevice&&) {
        row("device_close", "-");
    }

    // ---- verbs queue pairs and connections -----------------------------------
    void visit(Events::EventTrait<PE::IbvCreateQp>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::IbvCreateQp&&) {
        row("qp_create", "-");
    }
    void visit(Events::EventTrait<PE::IbvDestroyQp>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::IbvDestroyQp&&) {
        row("qp_destroy", "-");
    }
    void visit(Events::EventTrait<PE::IbvModifyQp>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::IbvModifyQp&&) {
        row("qp_modify", "-");
    }
    // Emitted by both SigmaTrace's verbs handler (modify_qp) and its DEVX handler
    // (a DEVX MODIFY_QP), so this is where DEVX queue pairs appear at all.
    void visit(Events::EventTrait<PE::QpConnectionInfo>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::QpConnectionInfo&& e) {
        row("qp_connect", "lid=%u port=%u", unsigned{e.dest_lid}, unsigned{e.port_num});
    }

    // ---- memory regions ------------------------------------------------------
    void visit(Events::EventTrait<PE::IbvMemoryReg>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::IbvMemoryReg&& e) {
        row("mr_reg", "size=%" PRIu64, e.size);
    }
    void visit(Events::EventTrait<PE::IbvMemoryDereg>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::IbvMemoryDereg&& e) {
        row("mr_dereg", "size=%" PRIu64, e.size);
    }

    // ---- DEVX umems and UARs -------------------------------------------------
    void visit(Events::EventTrait<PE::Mlx5dvMemoryRegistration>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::Mlx5dvMemoryRegistration&& e) {
        m_umem_size[e.mem_id] = e.size;
        row("umem_reg", "size=%" PRIu64, e.size);
    }
    void visit(Events::EventTrait<PE::Mlx5dvMemoryDereg>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::Mlx5dvMemoryDereg&& e) {
        const auto it = m_umem_size.find(e.mem_id);
        if (it == m_umem_size.end()) {
            row("umem_dereg", "size=?");
            return;
        }
        row("umem_dereg", "size=%" PRIu64, it->second);
        m_umem_size.erase(it);
    }
    void visit(Events::EventTrait<PE::Mlx5dvUarAlloc>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::Mlx5dvUarAlloc&& e) {
        row("uar_alloc", "flags=0x%x", e.flags);
    }
    void visit(Events::EventTrait<PE::Mlx5dvUarFree>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::Mlx5dvUarFree&&) {
        row("uar_free", "-");
    }

    // ---- data plane: expected empty under ibdevx on both sides ---------------
    void visit(Events::EventTrait<PE::IbvPostSend>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::IbvPostSend&& e) {
        row("post_send", "wrs=%u", e.num_wrs);
    }
    void visit(Events::EventTrait<PE::IbvPostRecv>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::IbvPostRecv&&) {
        row("post_recv", "-");
    }
    void visit(Events::EventTrait<PE::IbvCompletion>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::IbvCompletion&& e) {
        row("completion", "status=%d", static_cast<int>(e.status));
    }
    void visit(Events::EventTrait<PE::DoorbellRingData>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::DoorbellRingData&& e) {
        row("doorbell", "opcode=0x%02x", unsigned{e.opcode});
    }
    void visit(Events::EventTrait<PE::DoorbellRingControl>, const Events::EventFileHeader&,
               Events::EventHeader&&, PE::DoorbellRingControl&& e) {
        row("doorbell", "opcode=0x%02x", unsigned{e.opcode});
    }
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage: sigma_fingerprint <profiler-output-dir>\n", stderr);
        return 1;
    }
    Events::RuntimeConfig config{};
    config.OutputDirectory = argv[1];
    config.EventsDirectory = config.OutputDirectory / "events";
    config.MetaEventsDirectory = config.OutputDirectory / "meta";

    Fingerprint       reader{config};
    const std::size_t total = reader.process();
    std::fprintf(stderr, "sigma_fingerprint: %zu events from %s\n", total,
                 config.EventsDirectory.c_str());
    return 0;
}
