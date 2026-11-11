#pragma once

#include <bit>

#define comshu_ignore_unused(expr) (void)(expr)

namespace comshu::helpers {
static constexpr uint64_t PtrToUint(void* ptr) {
    return std::bit_cast<uint64_t>(ptr);
}
}  // namespace comshu::helpers
