// Binds libpcomm/schema/pcomm.fbs to the C++ record structs.
//
// A `.pcs` file is a StreamHeader followed by a packed array of Stamped<R>.
// Both are checked here. The schema declares the same
// layout as FlatBuffers structs, so a capture is readable from any language via
// flatc — but only while the two agree. Nothing else checks that: change a
// record and forget the schema, and pcomm keeps writing files that every
// non-C++ reader silently misparses. This test fails the build instead.
//
// Sizes are asserted at compile time. The .fbs is the canonical description; if
// one of these fires, update the schema (or add a new type_id — FlatBuffers
// structs cannot gain fields, and record types are already versioned).
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "pcomm_generated.h"

#include "pcomm/core/records.hpp"
#include "pcomm/rec/stream_header.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"
#include "ib_records.hpp"
#include "mlx5_records.hpp"

using pcomm::rec::Stamped;
namespace cr = pcomm::core::records;
namespace ir = pcomm::providers::ibverbs::records;
namespace mr = pcomm::providers::mlx5dv::records;
namespace fb = pcomm::fb;

#define PCOMM_SCHEMA_MATCH(CppRecord, FbStruct)                                                    \
    static_assert(sizeof(Stamped<CppRecord>) == sizeof(FbStruct),                                  \
                  #CppRecord " and pcomm.fbs " #FbStruct " disagree on size — "                    \
                             "update libpcomm/schema/pcomm.fbs")

PCOMM_SCHEMA_MATCH(cr::GpuAccess, fb::GpuAccess);
PCOMM_SCHEMA_MATCH(cr::MemObject, fb::MemObject);
PCOMM_SCHEMA_MATCH(cr::EntityClose, fb::EntityClose);
PCOMM_SCHEMA_MATCH(cr::SourceLine, fb::SourceLine);
PCOMM_SCHEMA_MATCH(cr::Attribution, fb::Attribution);
PCOMM_SCHEMA_MATCH(cr::NodeIdentity, fb::NodeIdentity);
PCOMM_SCHEMA_MATCH(cr::ClockAnchor, fb::ClockAnchor);
PCOMM_SCHEMA_MATCH(cr::InternKey, fb::InternKey);
PCOMM_SCHEMA_MATCH(cr::InternString, fb::InternString);
PCOMM_SCHEMA_MATCH(ir::IbMemRegion, fb::IbMemRegion);
PCOMM_SCHEMA_MATCH(ir::IbQueuePair, fb::IbQueuePair);
PCOMM_SCHEMA_MATCH(ir::IbCompletionQueue, fb::IbCompletionQueue);
PCOMM_SCHEMA_MATCH(ir::IbModifyQp, fb::IbModifyQp);
PCOMM_SCHEMA_MATCH(ir::IbPostSend, fb::IbPostSend);
PCOMM_SCHEMA_MATCH(ir::IbSharedRecvQueue, fb::IbSharedRecvQueue);
PCOMM_SCHEMA_MATCH(ir::IbPostRecv, fb::IbPostRecv);
PCOMM_SCHEMA_MATCH(ir::IbAddressHandle, fb::IbAddressHandle);
PCOMM_SCHEMA_MATCH(ir::IbDevice, fb::IbDevice);
PCOMM_SCHEMA_MATCH(ir::IbReqNotifyCq, fb::IbReqNotifyCq);
PCOMM_SCHEMA_MATCH(ir::IbPollCq, fb::IbPollCq);
PCOMM_SCHEMA_MATCH(ir::IbSrqRecvSlot, fb::IbSrqRecvSlot);
PCOMM_SCHEMA_MATCH(ir::IbRecvCqe, fb::IbRecvCqe);
PCOMM_SCHEMA_MATCH(mr::Mlx5Umem, fb::Mlx5Umem);
PCOMM_SCHEMA_MATCH(mr::Mlx5Uar, fb::Mlx5Uar);
PCOMM_SCHEMA_MATCH(mr::Mlx5DevxCmd, fb::Mlx5DevxCmd);
PCOMM_SCHEMA_MATCH(mr::Mlx5DevxQp, fb::Mlx5DevxQp);
PCOMM_SCHEMA_MATCH(mr::Mlx5DevxQpConnect, fb::Mlx5DevxQpConnect);
PCOMM_SCHEMA_MATCH(mr::Mlx5QueuePair, fb::Mlx5QueuePair);
PCOMM_SCHEMA_MATCH(mr::Mlx5Doorbell, fb::Mlx5Doorbell);
PCOMM_SCHEMA_MATCH(mr::Mlx5DeviceSend, fb::Mlx5DeviceSend);

// The file framing is on disk in every capture, so it is held to the same
// standard as the records: if StreamHeader and the schema disagree, every
// reader misreads the first 32 bytes of every stream and then misreads the
// records too, having taken the wrong stride from a header it got wrong.
static_assert(sizeof(pcomm::rec::StreamHeader) == sizeof(fb::StreamHeader),
              "StreamHeader and pcomm.fbs StreamHeader disagree on size");

// The shared vocabulary has to line up too, or every record inherits the skew.
static_assert(sizeof(pcomm::core::Temporal) == sizeof(fb::Temporal));
static_assert(sizeof(pcomm::core::MemRange) == sizeof(fb::MemRange));

// Enumerator values, one by one. Sizes and bytes cannot catch this: reorder a
// C++ enum and every record keeps its layout, its size and (for any single
// test vector) most of its bytes, while every reader that decodes the field by
// name now decodes it wrong. These are the only checks that see it, which is
// why the schema declares real enums instead of describing them in a comment.
#define PCOMM_SCHEMA_ENUM(CppEnumerator, FbEnumerator)                                             \
    static_assert(static_cast<std::uint8_t>(CppEnumerator) ==                                      \
                      static_cast<std::uint8_t>(FbEnumerator),                                     \
                  #CppEnumerator " and pcomm.fbs " #FbEnumerator " disagree — "                    \
                                 "the value is on disk; do not renumber either side")

PCOMM_SCHEMA_ENUM(pcomm::core::TimeKind::None, fb::TimeKind_None);
PCOMM_SCHEMA_ENUM(pcomm::core::TimeKind::Instant, fb::TimeKind_Instant);
PCOMM_SCHEMA_ENUM(pcomm::core::TimeKind::Interval, fb::TimeKind_Interval);
PCOMM_SCHEMA_ENUM(pcomm::core::TimeKind::Window, fb::TimeKind_Window);
PCOMM_SCHEMA_ENUM(pcomm::core::TimeKind::Counter, fb::TimeKind_Counter);

PCOMM_SCHEMA_ENUM(pcomm::core::RecordKind::Event, fb::RecordKind_Event);
PCOMM_SCHEMA_ENUM(pcomm::core::RecordKind::Entity, fb::RecordKind_Entity);
PCOMM_SCHEMA_ENUM(pcomm::core::RecordKind::Artifact, fb::RecordKind_Artifact);
PCOMM_SCHEMA_ENUM(pcomm::core::RecordKind::Edge, fb::RecordKind_Edge);

PCOMM_SCHEMA_ENUM(pcomm::core::MemOp::Load, fb::MemOp_Load);
PCOMM_SCHEMA_ENUM(pcomm::core::MemOp::Store, fb::MemOp_Store);
PCOMM_SCHEMA_ENUM(pcomm::core::MemOp::Atomic, fb::MemOp_Atomic);
PCOMM_SCHEMA_ENUM(pcomm::core::MemOp::Prefetch, fb::MemOp_Prefetch);

PCOMM_SCHEMA_ENUM(pcomm::core::MemSpace::Global, fb::MemSpace_Global);
PCOMM_SCHEMA_ENUM(pcomm::core::MemSpace::Local, fb::MemSpace_Local);
PCOMM_SCHEMA_ENUM(pcomm::core::MemSpace::Shared, fb::MemSpace_Shared);

PCOMM_SCHEMA_ENUM(pcomm::core::EdgePredicate::TouchedObject, fb::EdgePredicate_TouchedObject);
PCOMM_SCHEMA_ENUM(pcomm::core::EdgePredicate::MapsToSourceLine, fb::EdgePredicate_MapsToSourceLine);
PCOMM_SCHEMA_ENUM(pcomm::core::EdgePredicate::ResidesOn, fb::EdgePredicate_ResidesOn);
PCOMM_SCHEMA_ENUM(pcomm::core::EdgePredicate::MapsToQp, fb::EdgePredicate_MapsToQp);

// The asserts above only cover enumerators that exist on both sides, so an
// *added* value slips past them. flatc's *_MAX pins the schema-side count, and
// tools/schema_check.py compares the C++ enum bodies against the .fbs, so a new
// value on either side has to be added here deliberately.
static_assert(fb::TimeKind_MAX == 4 && fb::MemOp_MAX == 3 && fb::MemSpace_MAX == 2 &&
                  fb::EdgePredicate_MAX == 3 && fb::RecordKind_MAX == 3,
              "an enum gained a value in pcomm.fbs — add the matching C++ "
              "enumerator assertion above and update this count");

// sizeof alone is far too weak: two structs can share a size and have their
// fields in a different order, which is precisely the drift that would make
// every non-C++ reader misparse without any error. So also compare the actual
// bytes — fill a record, build the schema struct with the same values, and
// require them to be identical. This is what makes the .fbs binding rather
// than merely aspirational.
static int g_fail = 0;

static void expect_identical(const char* name, const void* a, const void* b, std::size_t n) {
    if (std::memcmp(a, b, n) == 0) {
        std::printf("  [ok ] %s\n", name);
        return;
    }
    std::printf("  [XX ] %s — bytes differ from libpcomm/schema/pcomm.fbs\n", name);
    const auto* p = static_cast<const unsigned char*>(a);
    const auto* q = static_cast<const unsigned char*>(b);
    for (std::size_t i = 0; i < n; ++i)
        if (p[i] != q[i])
            std::printf("        byte %3zu: cpp=%02x fbs=%02x\n", i, p[i], q[i]);
    g_fail = 1;
}

int main() {
    std::printf("schema conformance vs libpcomm/schema/pcomm.fbs\n");

    {  // GpuAccess: the firehose, and the one with the most padding holes
        Stamped<cr::GpuAccess> c{};
        c.temporal = pcomm::core::Temporal::instant({7, 0x1122334455667788ull});
        c.payload.pc = 0xAABBCCDD;
        c.payload.object = 0x11223344;
        c.payload.address = 0x0102030405060708ull;
        c.payload.size = 0xDEADBEEF;
        c.payload.op = static_cast<pcomm::core::MemOp>(3);
        c.payload.space = static_cast<pcomm::core::MemSpace>(2);
        c.payload.run_device = 0x1234;
        c.payload.mem_device = 0x5678;
        c.payload.thread = 0x99887766;
        fb::GpuAccess f(fb::Temporal(fb::TimeKind_Instant, fb::TimePoint(7, 0x1122334455667788ull),
                                     fb::TimePoint(0, 0), 0),
                        0xAABBCCDD, 0x11223344, 0x0102030405060708ull, 0xDEADBEEF,
                        fb::MemOp_Prefetch, fb::MemSpace_Shared, 0x1234, 0x5678, 0x99887766);
        expect_identical("GpuAccess", &c, &f, sizeof(c));
    }
    {  // IbPostSend: the network firehose, and it carries a real interval so
        // both TimePoints are populated
        Stamped<ir::IbPostSend> c{};
        c.temporal = pcomm::core::Temporal::interval({1, 0xAAAAull}, {2, 0xBBBBull});
        c.payload.qp = 0x0F0F0F0F;
        c.payload.wr_id = 0x1122334455667788ull;
        c.payload.remote_addr = 0x99AABBCCDDEEFF00ull;
        c.payload.rkey = 0xCAFEBABE;
        c.payload.length = 4096;
        c.payload.qp_num = 0x1EFFD;
        c.payload.opcode = 2;
        c.payload.num_sge = 1;
        fb::IbPostSend f(fb::Temporal(fb::TimeKind_Interval, fb::TimePoint(1, 0xAAAAull),
                                      fb::TimePoint(2, 0xBBBBull), 0),
                         0x0F0F0F0F, 0x1122334455667788ull, 0x99AABBCCDDEEFF00ull, 0xCAFEBABE, 4096,
                         0x1EFFD, 2, 1);
        expect_identical("IbPostSend", &c, &f, sizeof(c));
    }
    {
        Stamped<cr::MemObject> c{};
        c.temporal = pcomm::core::Temporal::interval({1, 0x111}, {2, 0x222});
        c.payload.id = 0xAABB;
        c.payload.name = 0xCCDD;
        c.payload.range = {0x1000, 0x2000};
        c.payload.device = 0x33;
        fb::MemObject f(fb::Temporal(fb::TimeKind_Interval, fb::TimePoint(1, 0x111),
                                     fb::TimePoint(2, 0x222), 0),
                        0xAABB, 0xCCDD, fb::MemRange(0x1000, 0x2000), 0x33);
        expect_identical("MemObject", &c, &f, sizeof(c));
    }
    {
        Stamped<cr::EntityClose> c{};
        c.temporal = pcomm::core::Temporal::interval({3, 0x333}, {4, 0x444});
        c.payload.subject = 0xBEEF;
        c.payload.opener = 100;
        fb::EntityClose f(fb::Temporal(fb::TimeKind_Interval, fb::TimePoint(3, 0x333),
                                       fb::TimePoint(4, 0x444), 0),
                          0xBEEF, 100);
        expect_identical("EntityClose", &c, &f, sizeof(c));
    }
    {
        Stamped<cr::SourceLine> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.pc = 0x1234;
        c.payload.file = 0x5678;
        c.payload.line = 42;
        fb::SourceLine f(
            fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0), 0x1234,
            0x5678, 42);
        expect_identical("SourceLine", &c, &f, sizeof(c));
    }
    {
        Stamped<cr::Attribution> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.subject = 7;
        c.payload.object = 9;
        c.payload.pred = static_cast<pcomm::core::EdgePredicate>(2);
        fb::Attribution f(
            fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0), 7, 9,
            fb::EdgePredicate_ResidesOn);
        expect_identical("Attribution", &c, &f, sizeof(c));
    }
    {
        Stamped<cr::NodeIdentity> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.node_hash = 0x0123456789ABCDEFull;
        fb::NodeIdentity f(
            fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0),
            0x0123456789ABCDEFull);
        expect_identical("NodeIdentity", &c, &f, sizeof(c));
    }
    {
        Stamped<cr::ClockAnchor> c{};
        c.temporal = pcomm::core::Temporal::instant({3, 0xABC});
        c.payload.domain = 3;
        c.payload.local_ticks = 0xDEAD;
        c.payload.wall_ns = 0xBEEF;
        c.payload.mono_ns = 0xF00D;
        fb::ClockAnchor f(
            fb::Temporal(fb::TimeKind_Instant, fb::TimePoint(3, 0xABC), fb::TimePoint(0, 0), 0), 3,
            0xDEAD, 0xBEEF, 0xF00D);
        expect_identical("ClockAnchor", &c, &f, sizeof(c));
    }
    {
        Stamped<ir::IbMemRegion> c{};
        c.temporal = pcomm::core::Temporal::interval({1, 0x10}, {1, 0x20});
        c.payload.mr = 1;
        c.payload.pd = 2;
        c.payload.range = {0x3000, 0x4000};
        c.payload.lkey = 5;
        c.payload.rkey = 6;
        c.payload.access = 7;
        c.payload.dmabuf_fd = -1;
        c.payload.origin = static_cast<std::uint8_t>(ir::IbRegionOrigin::RegMr);
        fb::IbMemRegion f(
            fb::Temporal(fb::TimeKind_Interval, fb::TimePoint(1, 0x10), fb::TimePoint(1, 0x20), 0),
            1, 2, fb::MemRange(0x3000, 0x4000), 5, 6, 7, -1,
            static_cast<uint8_t>(ir::IbRegionOrigin::RegMr));
        expect_identical("IbMemRegion", &c, &f, sizeof(c));
    }
    {
        Stamped<ir::IbQueuePair> c{};
        c.temporal = pcomm::core::Temporal::interval({1, 0x30}, {1, 0x40});
        c.payload.qp = 11;
        c.payload.pd = 12;
        c.payload.qp_num = 0x1EFFD;
        c.payload.qp_type = 2;
        fb::IbQueuePair f(
            fb::Temporal(fb::TimeKind_Interval, fb::TimePoint(1, 0x30), fb::TimePoint(1, 0x40), 0),
            11, 12, 0x1EFFD, 2);
        expect_identical("IbQueuePair", &c, &f, sizeof(c));
    }
    {
        Stamped<ir::IbCompletionQueue> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.cq = 13;
        c.payload.context = 14;
        c.payload.channel = 15;
        c.payload.cqe = 0x101;
        c.payload.comp_vector = 2;
        c.payload.origin = static_cast<uint8_t>(ir::IbCqOrigin::CreateCqEx);
        fb::IbCompletionQueue f(
            fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0), 13, 14,
            15, 0x101, 2, static_cast<uint8_t>(ir::IbCqOrigin::CreateCqEx));
        expect_identical("IbCompletionQueue", &c, &f, sizeof(c));
    }
    {
        Stamped<ir::IbModifyQp> c{};
        c.temporal = pcomm::core::Temporal::instant({1, 0x50});
        c.payload.qp = 21;
        c.payload.qp_num = 0x1EFFD;
        c.payload.dest_qp_num = 0x1EFFC;
        c.payload.dest_lid = 0xA93;
        c.payload.src_lid = 0x1F0E;
        c.payload.state = 3;
        fb::IbModifyQp f(
            fb::Temporal(fb::TimeKind_Instant, fb::TimePoint(1, 0x50), fb::TimePoint(0, 0), 0), 21,
            0x1EFFD, 0x1EFFC, 0xA93, 0x1F0E, 3);
        expect_identical("IbModifyQp", &c, &f, sizeof(c));
    }
    {
        Stamped<ir::IbSharedRecvQueue> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.srq = 51;
        c.payload.pd = 52;
        c.payload.max_wr = 0x200;
        c.payload.max_sge = 4;
        c.payload.srq_limit = 0x10;
        fb::IbSharedRecvQueue f(
            fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0), 51, 52,
            0x200, 4, 0x10);
        expect_identical("IbSharedRecvQueue", &c, &f, sizeof(c));
    }
    {
        Stamped<ir::IbPostRecv> c{};
        c.temporal = pcomm::core::Temporal::interval({1, 0x80}, {1, 0x90});
        c.payload.subject = 41;
        c.payload.wr_id = 0xCAFEBABE;
        c.payload.addr = 0x7F0000112233;
        c.payload.lkey = 0x2B;
        c.payload.length = 8192;
        c.payload.qp_num = 0x1EFFB;
        c.payload.num_sge = 1;
        c.payload.origin = static_cast<uint8_t>(ir::IbRecvOrigin::QueuePair);
        fb::IbPostRecv f(
            fb::Temporal(fb::TimeKind_Interval, fb::TimePoint(1, 0x80), fb::TimePoint(1, 0x90), 0),
            41, 0xCAFEBABE, 0x7F0000112233, 0x2B, 8192, 0x1EFFB, 1,
            static_cast<uint8_t>(ir::IbRecvOrigin::QueuePair));
        expect_identical("IbPostRecv", &c, &f, sizeof(c));
    }
    {
        Stamped<ir::IbAddressHandle> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.ah = 71;
        c.payload.pd = 72;
        c.payload.dgid_prefix = 0xFE80000000000000ull;
        c.payload.dgid_iface = 0x0002C90300ABCDEFull;
        c.payload.flow_label = 0x12345;
        c.payload.dlid = 0xA93;
        c.payload.sl = 3;
        c.payload.port_num = 1;
        c.payload.is_global = 1;
        c.payload.sgid_index = 5;
        c.payload.hop_limit = 64;
        c.payload.traffic_class = 7;
        fb::IbAddressHandle f(
            fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0), 71, 72,
            0xFE80000000000000ull, 0x0002C90300ABCDEFull, 0x12345, 0xA93, 3, 1, 1, 5, 64, 7);
        expect_identical("IbAddressHandle", &c, &f, sizeof(c));
    }
    {
        Stamped<ir::IbDevice> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.context = 81;
        c.payload.fw_ver = 82;
        c.payload.node_guid = 0x0002C90300ABCDEFull;
        c.payload.sys_image_guid = 0x0002C90300ABCDF0ull;
        c.payload.vendor_id = 0x02C9;
        c.payload.vendor_part_id = 4123;
        c.payload.hw_ver = 0;
        c.payload.phys_port_cnt = 1;
        fb::IbDevice f(fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0),
                       81, 82, 0x0002C90300ABCDEFull, 0x0002C90300ABCDF0ull, 0x02C9, 4123, 0, 1);
        expect_identical("IbDevice", &c, &f, sizeof(c));
    }
    {
        Stamped<ir::IbReqNotifyCq> c{};
        c.temporal = pcomm::core::Temporal::interval({1, 0xA0}, {1, 0xB0});
        c.payload.cq = 61;
        c.payload.solicited_only = 1;
        fb::IbReqNotifyCq f(
            fb::Temporal(fb::TimeKind_Interval, fb::TimePoint(1, 0xA0), fb::TimePoint(1, 0xB0), 0),
            61, 1);
        expect_identical("IbReqNotifyCq", &c, &f, sizeof(c));
    }
    {
        Stamped<ir::IbPollCq> c{};
        c.temporal = pcomm::core::Temporal::interval({1, 0x60}, {1, 0x70});
        c.payload.cq = 31;
        c.payload.wr_id = 0xFEEDFACE;
        c.payload.byte_len = 4096;
        c.payload.qp_num = 0x1EFFD;
        c.payload.status = 0;
        c.payload.opcode = 1;
        fb::IbPollCq f(
            fb::Temporal(fb::TimeKind_Interval, fb::TimePoint(1, 0x60), fb::TimePoint(1, 0x70), 0),
            31, 0xFEEDFACE, 4096, 0x1EFFD, 0, 1);
        expect_identical("IbPollCq", &c, &f, sizeof(c));
    }
    {
        Stamped<ir::IbSrqRecvSlot> c{};
        c.temporal = pcomm::core::Temporal::interval({1, 0x62}, {1, 0x72});
        c.payload.srq = 33;
        c.payload.wr_id = 0xC0FFEE11;
        c.payload.addr = 0x7F1200340000;
        c.payload.srqn = 0x2A;
        c.payload.slot = 0x3B;
        fb::IbSrqRecvSlot f(
            fb::Temporal(fb::TimeKind_Interval, fb::TimePoint(1, 0x62), fb::TimePoint(1, 0x72), 0),
            33, 0xC0FFEE11, 0x7F1200340000, 0x2A, 0x3B);
        expect_identical("IbSrqRecvSlot", &c, &f, sizeof(c));
    }
    {
        // srq and wqe_counter are adjacent uint32s, so they are given values
        // that cannot be confused for each other — a swapped pair is precisely
        // what a byte comparison exists to catch, and this record's whole
        // purpose is that those two fields mean different things.
        Stamped<ir::IbRecvCqe> c{};
        c.temporal = pcomm::core::Temporal::interval({1, 0x64}, {1, 0x74});
        c.payload.cq = 35;
        c.payload.wr_id = 0xDEADBEEF01;
        c.payload.srq = 0x4C;
        c.payload.wqe_counter = 0x5D;
        c.payload.qp_num = 0x6EFFD;
        c.payload.byte_len = 8192;
        fb::IbRecvCqe f(
            fb::Temporal(fb::TimeKind_Interval, fb::TimePoint(1, 0x64), fb::TimePoint(1, 0x74), 0),
            35, 0xDEADBEEF01, 0x4C, 0x5D, 0x6EFFD, 8192);
        expect_identical("IbRecvCqe", &c, &f, sizeof(c));
    }
    // --- mlx5dv/DEVX (200-299) ---------------------------------------------
    // Values chosen to be distinguishable per field rather than round: a
    // reordered pair of same-width fields is exactly what byte comparison is
    // here to catch, and equal values would hide it.
    {
        Stamped<mr::Mlx5Umem> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.umem = 41;
        c.payload.range = {0xDEAD0000, 0x50000};
        c.payload.umem_id = 0x2A;
        c.payload.access = 0x7;
        c.payload.dmabuf_fd = -1;
        c.payload.device = 3;
        c.payload.binding = 0;
        c.payload.entry = 1;
        fb::Mlx5Umem f(fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0),
                       41, fb::MemRange(0xDEAD0000, 0x50000), 0x2A, 0x7, -1, 3, 0, 1);
        expect_identical("Mlx5Umem", &c, &f, sizeof(c));
    }
    {
        Stamped<mr::Mlx5Uar> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.uar = 42;
        c.payload.reg_addr = 0x7F0000001800;
        c.payload.base_addr = 0x7F0000001000;
        c.payload.page_id = 275;
        c.payload.flags = 1;
        fb::Mlx5Uar f(fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0),
                      42, 0x7F0000001800, 0x7F0000001000, 275, 1);
        expect_identical("Mlx5Uar", &c, &f, sizeof(c));
    }
    {
        Stamped<mr::Mlx5DevxCmd> c{};
        c.temporal = pcomm::core::Temporal::instant({1, 0x80});
        c.payload.obj = 43;
        c.payload.opcode = 0x500;
        c.payload.decoded = 1;
        fb::Mlx5DevxCmd f(
            fb::Temporal(fb::TimeKind_Instant, fb::TimePoint(1, 0x80), fb::TimePoint(0, 0), 0), 43,
            0x500, 1);
        expect_identical("Mlx5DevxCmd", &c, &f, sizeof(c));
    }
    {
        Stamped<mr::Mlx5DevxQp> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.obj = 44;
        c.payload.sq_base = 0x7FAB00000000;
        c.payload.qp_num = 0x1EFFD;
        c.payload.wq_umem_id = 0x2A;
        c.payload.log_sq_size = 10;
        c.payload.device = 2;
        c.payload.service_type = 0x5;
        c.payload.gpu_driven = 1;
        fb::Mlx5DevxQp f(
            fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0), 44,
            0x7FAB00000000, 0x1EFFD, 0x2A, 10, 2, 0x5, 1);
        expect_identical("Mlx5DevxQp", &c, &f, sizeof(c));
    }
    {
        Stamped<mr::Mlx5DevxQpConnect> c{};
        c.temporal = pcomm::core::Temporal::instant({1, 0x90});
        c.payload.dest_gid_prefix = 0xFE80000000000000ull;
        c.payload.dest_gid_iface = 0x0002C90300ABCDEFull;
        c.payload.qp_num = 0x1EFFD;
        c.payload.dest_qp_num = 0x1EFFC;
        c.payload.dest_lid = 0xA93;
        c.payload.src_lid = 0x1F0E;
        c.payload.port_num = 1;
        fb::Mlx5DevxQpConnect f(
            fb::Temporal(fb::TimeKind_Instant, fb::TimePoint(1, 0x90), fb::TimePoint(0, 0), 0),
            0xFE80000000000000ull, 0x0002C90300ABCDEFull, 0x1EFFD, 0x1EFFC, 0xA93, 0x1F0E, 1);
        expect_identical("Mlx5DevxQpConnect", &c, &f, sizeof(c));
    }
    {
        Stamped<mr::Mlx5QueuePair> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.qp = 51;
        c.payload.pd = 52;
        c.payload.qp_num = 0x1EFFB;
        c.payload.qp_type = 2;
        fb::Mlx5QueuePair f(
            fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0), 51, 52,
            0x1EFFB, 2);
        expect_identical("Mlx5QueuePair", &c, &f, sizeof(c));
    }
    {
        Stamped<mr::Mlx5Doorbell> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.uar = 0x2A;
        c.payload.qp_num = 0x1EFFA;
        c.payload.prod_idx = 0x0140;
        fb::Mlx5Doorbell f(
            fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0), 0x2A,
            0x1EFFA, 0x0140);
        expect_identical("Mlx5Doorbell", &c, &f, sizeof(c));
    }
    {
        Stamped<mr::Mlx5DeviceSend> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.laddr = 0x7F0011220000ull;
        c.payload.raddr = 0x7F0099880000ull;
        c.payload.qp_num = 0x1EFF9;
        c.payload.wqe_idx = 0x0080;
        c.payload.lkey = 0xABCD;
        c.payload.rkey = 0xDCBA;
        c.payload.length = 4096;
        c.payload.opcode = 0x08;
        fb::Mlx5DeviceSend f(
            fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0),
            0x7F0011220000ull, 0x7F0099880000ull, 0x1EFF9, 0x0080, 0xABCD, 0xDCBA, 4096, 0x08);
        expect_identical("Mlx5DeviceSend", &c, &f, sizeof(c));
    }
    {  // InternKey / InternString: the dictionary every Handle depends on.
        Stamped<cr::InternKey> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.key = 0x7F00'1234'5678'9ABCull;
        c.payload.handle = 0xCAFEBABE;
        c.payload.domain = 3;
        fb::InternKey f(
            fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0),
            0x7F00'1234'5678'9ABCull, 0xCAFEBABE, 3);
        expect_identical("InternKey", &c, &f, sizeof(c));
    }
    {
        Stamped<cr::InternString> c{};
        c.temporal = pcomm::core::Temporal::none();
        c.payload.offset = 0x1122334455667788ull;
        c.payload.length = 0xDEADBEEF;
        c.payload.handle = 0x0F0F0F0F;
        c.payload.domain = 2;
        fb::InternString f(
            fb::Temporal(fb::TimeKind_None, fb::TimePoint(0, 0), fb::TimePoint(0, 0), 0),
            0x1122334455667788ull, 0xDEADBEEF, 0x0F0F0F0F, 2);
        expect_identical("InternString", &c, &f, sizeof(c));
    }
    {  // BlobHeader: the other file framing.
        pcomm::rec::BlobHeader c;
        c.byte_count = 0x0102030405060708ull;
        fb::BlobHeader f(pcomm::rec::kBlobMagic, pcomm::rec::kStreamFormatVersion,
                         0x0102030405060708ull, pcomm::rec::kSchemaHash);
        expect_identical("BlobHeader", &c, &f, sizeof(c));
    }
    {  // StreamHeader: not a record, but the first thing in every file.
        pcomm::rec::StreamHeader c(710, pcomm::core::RecordKind::Entity, 0xDEADBEEF);
        c.schema_hash = 0x0123456789ABCDEFull;
        fb::StreamHeader f(pcomm::rec::kStreamMagic, pcomm::rec::kStreamFormatVersion, 710,
                           0xDEADBEEF, 0x0123456789ABCDEFull, fb::RecordKind_Entity);
        expect_identical("StreamHeader", &c, &f, sizeof(c));
    }
    std::printf("  sizes: Stamped<GpuAccess]=%zu Stamped<IbPostSend]=%zu Temporal=%zu\n",
                sizeof(Stamped<cr::GpuAccess>), sizeof(Stamped<ir::IbPostSend>),
                sizeof(pcomm::core::Temporal));
    std::printf("%s\n",
                g_fail ? "FAIL"
                       : "PASS — all 12 records + both file headers byte-identical to the schema");
    return g_fail;
}
