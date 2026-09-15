#pragma once
// Providers carried INSIDE the launcher, and unpacked on demand.
//
// THE PROBLEM. `cmake --install` gives a copyable bin/ + lib/pcomm/ tree, but
// only for the targets of the tree it was given — and this project has four
// build trees by necessity (NVBit pins gcc-11.4 for gpumem, UCX needs the HPC-X
// tree, NCCL its headers), whose libraries are *copied* into build/providers/
// by tests/hpc/run_on_mn5.sh. A copied file is not an install target, so the
// bundle silently carries fewer providers than the build it came from. Four of
// the seven were missing.
//
// So the binary carries them. `pcomm bake -o pcomm.new pcomm <libs...>` appends
// the libraries to a copy of the launcher; at run time the launcher unpacks the
// ones a run actually selected into a cache directory and preloads from there.
// One file to copy to a cluster, providers included.
//
// APPENDED, NOT AN ELF SECTION, and the trade is worth stating. `objcopy
// --add-section` is the tidier answer and readelf can list it — but a
// non-allocated section is exactly what `strip` deletes, and the payload would
// vanish from a binary someone stripped to make it smaller. A blob past the end
// of the ELF image survives strip, objcopy and cp; it does not survive a tool
// that REWRITES the file (strip does, when it has reason to rewrite rather than
// truncate). Either way the failure is visible — `pcomm list` says what is
// carried — and neither shape is silently wrong.
//
// LAYOUT, low to high, all integers little-endian (same machine writes and
// reads; a cross-endian bake would be a cross-compile, which nothing here does):
//
//   [ host binary, exactly as it was ]
//   [ blob 0 ][ blob 1 ] ... [ blob n-1 ]
//   [ TOC   ] n × { u32 name_len, name bytes, u64 offset, u64 size, u64 hash,
//                   u64 mtime }
//   [ footer, 40 bytes ] u64 host_size, u64 toc_offset, u64 toc_size,
//                        u32 count, u32 format_version, char magic[8]
//
// `host_size` is what makes re-baking exact rather than cumulative: a second
// bake truncates to it instead of appending a second archive behind the first.
//
// `mtime` (format 2) is the source file's modification time when it was baked,
// in seconds, and every unpacked copy is stamped with it. Format 1 left each copy
// with the time it was UNPACKED, and Invocation.images — which proves a mapped
// provider was built with the engine that loaded it — reads mtimes: a baked
// engine unpacked fourteen minutes after the build looked fourteen minutes
// newer than a provider loaded straight from the build tree, and
// images_are_no_older_than_the_engine failed a correct capture (MN5 job
// 45894230, case 05: NCCL's profiler plugin from build/, 873 s "older").
#include <string>
#include <vector>

namespace pcomm::baked {

struct Entry {
    std::string        name;    // basename, e.g. "libucx_trace.so"
    unsigned long long offset;  // from the start of the file
    unsigned long long size;
    unsigned long long hash;   // FNV-1a of the blob; identifies the cache dir
    long long          mtime;  // source mtime at bake, seconds; stamped on unpack
};

// What this binary carries. Empty when nothing was baked in, which is the
// normal state of a build-tree launcher. Read once from /proc/self/exe.
const std::vector<Entry>& contents();

// Unpack `name` (a basename) and return an absolute path to it, or "" when this
// binary does not carry it. Files land in a cache directory keyed by the
// CONTENT of the archive, so two different builds never share a cached copy —
// the staleness this project keeps being bitten by is not worth re-inventing
// here. Repeated calls, and concurrent ranks on one node, reuse the same copy.
//
//   $PCOMM_BAKE_DIR   where to unpack (default $TMPDIR, else /tmp)
std::string materialize(const std::string& name);

// `pcomm bake` — see the usage text in baked.cpp.
int bake_main(int argc, char** argv);

}  // namespace pcomm::baked
