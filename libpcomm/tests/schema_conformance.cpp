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

int main() {
    std::printf("schema conformance: 10 records + 2 vocabulary types match "
                "libpcomm/schema/pcomm.fbs\n");
    std::printf("  Stamped<GpuAccess]=%zu  Stamped<IbPostSend]=%zu  Temporal=%zu\n",
                sizeof(Stamped<cr::GpuAccess>), sizeof(Stamped<ir::IbPostSend>),
                sizeof(pcomm::core::Temporal));
    return 0;
}
