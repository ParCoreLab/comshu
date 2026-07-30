// Binds libpcomm/schema/pcomm.fbs to the C++ record structs.
//
// A `.pcs` file is a packed array of Stamped<R>. The schema declares the same
// layout as FlatBuffers structs, so a capture is readable from any language via
// flatc — but only while the two agree. Nothing else checks that: change a
// record and forget the schema, and pcomm keeps writing files that every
// non-C++ reader silently misparses. This test fails the build instead.
//
// Sizes are asserted at compile time. The .fbs is the canonical description; if
// one of these fires, update the schema (or add a new type_id — FlatBuffers
// structs cannot gain fields, and record types are already versioned).
#include <cstdio>
#include <cstring>

#include "pcomm_generated.h"

#include "pcomm/core/records.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"
#include "ib_records.hpp"

using pcomm::rec::Stamped;
namespace cr = pcomm::core::records;
namespace ir = pcomm::providers::ibverbs::records;
namespace fb = pcomm::fb;

#define PCOMM_SCHEMA_MATCH(CppRecord, FbStruct)                                \
    static_assert(sizeof(Stamped<CppRecord>) == sizeof(FbStruct),              \
                  #CppRecord " and pcomm.fbs " #FbStruct " disagree on size — " \
                  "update libpcomm/schema/pcomm.fbs")

PCOMM_SCHEMA_MATCH(cr::GpuAccess,   fb::GpuAccess);
PCOMM_SCHEMA_MATCH(cr::MemObject,   fb::MemObject);
PCOMM_SCHEMA_MATCH(cr::SourceLine,  fb::SourceLine);
PCOMM_SCHEMA_MATCH(cr::Attribution, fb::Attribution);
PCOMM_SCHEMA_MATCH(cr::ClockAnchor, fb::ClockAnchor);
PCOMM_SCHEMA_MATCH(ir::IbMemRegion, fb::IbMemRegion);
PCOMM_SCHEMA_MATCH(ir::IbQueuePair, fb::IbQueuePair);
PCOMM_SCHEMA_MATCH(ir::IbModifyQp,  fb::IbModifyQp);
PCOMM_SCHEMA_MATCH(ir::IbPostSend,  fb::IbPostSend);
PCOMM_SCHEMA_MATCH(ir::IbPollCq,    fb::IbPollCq);

// The shared vocabulary has to line up too, or every record inherits the skew.
static_assert(sizeof(pcomm::core::Temporal) == sizeof(fb::Temporal));
static_assert(sizeof(pcomm::core::MemRange) == sizeof(fb::MemRange));

// sizeof alone is far too weak: two structs can share a size and have their
// fields in a different order, which is precisely the drift that would make
// every non-C++ reader misparse without any error. So also compare the actual
// bytes — fill a record, build the schema struct with the same values, and
// require them to be identical. This is what makes the .fbs binding rather
// than merely aspirational.
static int g_fail = 0;

static void expect_identical(const char* name, const void* a, const void* b,
                             std::size_t n) {
    if (std::memcmp(a, b, n) == 0) { std::printf("  [ok ] %s\n", name); return; }
    std::printf("  [XX ] %s — bytes differ from libpcomm/schema/pcomm.fbs\n", name);
    const auto* p = static_cast<const unsigned char*>(a);
    const auto* q = static_cast<const unsigned char*>(b);
    for (std::size_t i = 0; i < n; ++i)
        if (p[i] != q[i]) std::printf("        byte %3zu: cpp=%02x fbs=%02x\n", i, p[i], q[i]);
    g_fail = 1;
}

int main() {
    std::printf("schema conformance vs libpcomm/schema/pcomm.fbs\n");

    {   // GpuAccess: the firehose, and the one with the most padding holes
        Stamped<cr::GpuAccess> c{};
        c.temporal = pcomm::core::Temporal::instant({7, 0x1122334455667788ull});
        c.payload.pc = 0xAABBCCDD; c.payload.object = 0x11223344;
        c.payload.address = 0x0102030405060708ull; c.payload.size = 0xDEADBEEF;
        c.payload.op = static_cast<pcomm::core::MemOp>(3);
        c.payload.space = static_cast<pcomm::core::MemSpace>(2);
        c.payload.run_device = 0x1234; c.payload.mem_device = 0x5678;
        c.payload.thread = 0x99887766;
        fb::GpuAccess f(fb::Temporal(1, fb::TimePoint(7, 0x1122334455667788ull),
                                     fb::TimePoint(0, 0), 0),
                        0xAABBCCDD, 0x11223344, 0x0102030405060708ull, 0xDEADBEEF,
                        3, 2, 0x1234, 0x5678, 0x99887766);
        expect_identical("GpuAccess", &c, &f, sizeof(c));
    }
    {   // IbPostSend: the network firehose, and it carries a real interval so
        // both TimePoints are populated
        Stamped<ir::IbPostSend> c{};
        c.temporal = pcomm::core::Temporal::interval({1, 0xAAAAull}, {2, 0xBBBBull});
        c.payload.qp = 0x0F0F0F0F; c.payload.wr_id = 0x1122334455667788ull;
        c.payload.remote_addr = 0x99AABBCCDDEEFF00ull; c.payload.rkey = 0xCAFEBABE;
        c.payload.length = 4096; c.payload.qp_num = 0x1EFFD;
        c.payload.opcode = 2; c.payload.num_sge = 1;
        fb::IbPostSend f(fb::Temporal(2, fb::TimePoint(1, 0xAAAAull),
                                      fb::TimePoint(2, 0xBBBBull), 0),
                         0x0F0F0F0F, 0x1122334455667788ull, 0x99AABBCCDDEEFF00ull,
                         0xCAFEBABE, 4096, 0x1EFFD, 2, 1);
        expect_identical("IbPostSend", &c, &f, sizeof(c));
    }
    std::printf("  sizes: Stamped<GpuAccess]=%zu Stamped<IbPostSend]=%zu Temporal=%zu\n",
                sizeof(Stamped<cr::GpuAccess>), sizeof(Stamped<ir::IbPostSend>),
                sizeof(pcomm::core::Temporal));
    std::printf("%s\n", g_fail ? "FAIL" : "PASS (sizes for 10 records, bytes for 2)");
    return g_fail;
}
