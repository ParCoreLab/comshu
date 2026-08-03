#pragma once

#include <bit>

#define pcomm_ignore_unused(expr) (void)(expr)

namespace pcomm::helpers {
static constexpr uint64_t PtrToUint(void* ptr) {
    return std::bit_cast<uint64_t>(ptr);
}
}  // namespace pcomm::helpers
