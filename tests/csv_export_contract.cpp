// Writes a tiny capture whose free-text columns contain commas and quotes —
// a demangled template kernel and an object named like a C++ type — for
// tests/csv_export_contract.sh (CR-28, CR-29).
#include <cstdint>
#include <filesystem>

#include "comshu/core/records.hpp"
#include "comshu/engine_abi.h"
#include "comshu/rec/intern_table.hpp"
#include "comshu/tracer.hpp"

int main(int argc, char** argv) {
    if (argc < 2)
        return 2;
    comshu::Config cfg;
    cfg.output_dir = argv[1];
    cfg.rank = 0;
    comshu::Tracer::instance().start(cfg);
    auto&                  r = comshu::Tracer::instance().recorder();
    const comshu::core::Handle name =
        r.intern(comshu::rec::InternDomain::String, "std::pair<int, \"q\">");
    r.record(comshu::core::records::MemObject{{}, {}, 1, name, {0x1000, 64}, 0});
    const std::uint32_t grid[3] = {1, 1, 1};
    const std::uint16_t block[3] = {32, 1, 1};
    comshu_engine_record_kernel(10, 20, 0, "void k<int, float>(int, float)", 7, 0, 0, 0, grid,
                                block);
    comshu::Tracer::instance().stop();
    return 0;
}
