// Are the two layouts bit-identical? If yes, adopting FB structs is a drop-in:
// existing .pcs files stay readable and every golden holds.
#include <cstdio>
#include <cstring>
#include "gpuaccess_generated.h"
#include "pcomm/core/records.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"
using namespace pcomm;
using Rec = rec::Stamped<core::records::GpuAccess>;
int main(){
  Rec a{};
  std::memset(&a, 0, sizeof(a));   // zero padding, then assign fields
  a.temporal = core::Temporal::instant({7, 0x1122334455667788ull});
  a.payload.pc = 0xAABBCCDD; a.payload.object = 0x11223344;
  a.payload.address = 0x0102030405060708ull; a.payload.size = 0xDEADBEEF;
  a.payload.op = static_cast<core::MemOp>(3); a.payload.space = static_cast<core::MemSpace>(2);
  a.payload.run_device = 0x1234; a.payload.mem_device = 0x5678; a.payload.thread = 0x99887766;

  pcomm::fb::GpuAccessF b(
    pcomm::fb::TemporalF(static_cast<uint8_t>(core::TimeKind::Instant),
                         pcomm::fb::TimePointF(7, 0x1122334455667788ull),
                         pcomm::fb::TimePointF(0, 0), 0),
    0xAABBCCDD, 0x11223344, 0x0102030405060708ull, 0xDEADBEEF,
    3, 2, 0x1234, 0x5678, 0x99887766);

  std::printf("sizeof: pcomm=%zu fb=%zu   (padding pre-zeroed)\n", sizeof(a), sizeof(b));
  int diff = std::memcmp(&a, &b, sizeof(a) < sizeof(b) ? sizeof(a) : sizeof(b));
  std::printf("memcmp = %d  -> %s\n", diff, diff == 0 ? "BIT-IDENTICAL" : "DIFFERS");
  if (diff) {
    const unsigned char *p = (const unsigned char*)&a, *q = (const unsigned char*)&b;
    for (size_t i = 0; i < sizeof(a); ++i)
      if (p[i] != q[i]) std::printf("  byte %2zu: pcomm=%02x fb=%02x\n", i, p[i], q[i]);
  }
}
