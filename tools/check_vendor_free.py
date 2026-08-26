#!/usr/bin/env python3
"""Refuse a vendor type that has leaked into libpcomm.

README states the layer rule absolutely: `libpcomm/` is L0-L2 core and is
vendor-free -- no cuda*/hip*/ibv_*/Sanitizer_* types allowed; vendor types live
in `providers/` and `gpu-backends/`. Until now that was a sentence in a README
with nothing checking it, which is the same shape as items 8, 9 and 12 in
plans/todos.txt: a convention held by discipline rather than by construction.

WHY THIS IS NOT A GREP. Vendor names appear on nearly every page of libpcomm --
in COMMENTS, which are where this codebase keeps its reasoning. `mem_attr.hpp`
explains that its constants are hand-copied from cuda.h; `dmabuf_registry.hpp`
cites nvshmem's cudawrap.cpp by line; `CompositeHookManager.hpp` names
ibv_create_qp and mlx5dv_create_qp as the doors it exists to cover. A grep over
raw text reports every one of those and is therefore useless -- a check nobody
can keep green gets deleted, and then the rule is unenforced again with an extra
file to explain. Comments naming vendor doors are the point of the file. So is
`dlsym(lib, "cuPointerGetAttribute")`: borrowing an entry point BY NAME, with no
header and no link, is precisely how core stays vendor-free.

So this strips comments and string literals first and checks what is left --
declarations, types, calls. What survives that filter is a real leak.

The two files that come closest to the line pass deliberately, and how they pass
is the rule in miniature:

    mem_attr.hpp        declares `using CuPointerGetAttributeFn = int (*)(...)`
                        instead of including cuda.h for the prototype
    dmabuf_registry.hpp writes `using CuResult = int;` -- cuda.h's CUresult
                        renamed, so the ABI is copied and the NAME is not

Both carry the reasoning in the comment above the declaration. That is the
pattern a new one should follow.

WHAT THIS DOES NOT CATCH, and cannot. `mem_attr.hpp` hardcodes
kAttrDeviceOrdinal = 9 -- a vendor ABI constant with no vendor type in sight.
The letter of the rule holds and the dependency is real; its own comment records
that a wrong constant (4, CU_POINTER_ATTRIBUTE_HOST_POINTER) survived a
provider's entire lifetime because every failure mode looks like
kUnknownDevice. No regex can check a magic number against a header it is
forbidden to include. That gap is named in plans/todos.txt item 10 and stays
open; this check covers the part that IS mechanical.

    tools/check_vendor_free.py          # exit 1 and print every leak

Registered in ctest as `vendor_free`.
"""

import re
import subprocess
import sys
from pathlib import Path

# The rule's scope. providers/ and gpu-backends/ are where vendor types belong.
ROOT_DIR = "libpcomm"
SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp", ".cu", ".cuh", ".inc"}

# Generated FlatBuffers bindings are machine-written from our own pcomm.fbs and
# are checked at their source instead. They also carry schema FIELD names like
# `cuda_dev` -- a field meaning "which CUDA device", which is vocabulary and not
# a vendor type. Excluding the directory says that once, here, rather than
# teaching every pattern below to spare it.
EXCLUDE_DIRS = ("libpcomm/schema/generated/",)

# Vendor headers. Including one is a leak regardless of what is then used from
# it, and it is the check that cannot be argued with: core must compile on a
# host with no CUDA and no verbs at all, which is MN5's own configuration.
FORBIDDEN_INCLUDES = re.compile(
    r'^\s*#\s*include\s*[<"]\s*('
    r"cuda\w*\.h|driver_types\.h|cudaTypedefs\.h|"
    r"infiniband/[\w/]+\.h|rdma/[\w/]+\.h|"
    r"hip/[\w/]+\.h|"
    r"sanitizer\w*\.h|"
    r"nccl\w*\.h|nvshmem\w*\.h|nvbit\w*\.h|cupti\w*\.h|"
    r"ucp/[\w/]+\.h|uct/[\w/]+\.h|ucs/[\w/]+\.h"
    r')\s*[>"]',
    re.IGNORECASE,
)

# TYPES. Never allowed, anywhere in core, for any reason. A vendor type in a
# declaration means core cannot be compiled without that vendor's SDK, and
# "core + tests build with no CUDA/verbs installed" (README) is the property the
# whole layering exists to protect -- MN5's own core partition is such a host.
#
# Each pattern is written to spare the deliberate workarounds:
#   CU[a-z]      catches CUresult/CUdeviceptr/CUstream, NOT `CuResult`
#   cuda[A-Z]    catches cudaMalloc/cudaError_t, NOT the field name `cuda_dev`
# The verbs entries want `struct ibv_qp` and `ibv_qp*` -- a bare `ibv_foo` is a
# function name and is judged as a symbol below, not as a type.
VENDOR_TYPES = [
    (re.compile(r"\bCU[a-z]\w*\b"), "CUDA driver type"),
    (re.compile(r"\bCUDA_\w+\b"), "CUDA driver constant"),
    (re.compile(r"\bcuda[A-Z]\w*\b"), "CUDA runtime type or symbol"),
    (re.compile(r"\bCUpti_\w+\b"), "CUPTI type"),
    (re.compile(r"\bhip[A-Z]\w*\b|\bhip\w+_t\b"), "HIP type"),
    (re.compile(r"\bSanitizer_\w+\b|\bsanitizer[A-Z]\w*\b"), "Compute-Sanitizer type"),
    (re.compile(r"\bnccl[A-Z]\w*\b"), "NCCL type or symbol"),
    (re.compile(r"\bstruct\s+(?:ibv|mlx5dv)_\w+"), "verbs struct"),
    (re.compile(r"\b(?:ibv|mlx5dv)_\w+\s*\*"), "pointer to a verbs struct"),
    (re.compile(r"\b(?:ucp|uct|ucs)_\w+_[th]\b"), "UCX type"),
]

# SYMBOLS. Naming one is not automatically a leak, and this is the distinction
# the README sentence flattens. Interposition and dlsym work BY NAME: an
# `extern "C" void nvshmem_free(void* p)` needs no nvshmem header, takes no
# vendor type, and links against nothing -- the dynamic linker routes the
# application's call through it. That is a different act from depending on a
# vendor's ABI, and core does it deliberately in exactly one place.
#
# So a symbol is checked against ALLOWED_SYMBOLS instead of banned outright.
# The value of the list is not the ban; it is that a SIXTH door appearing in the
# engine has to be written down here. A provider-specific door drifting into
# core is a layering violation, and it is invisible without this.
VENDOR_SYMBOLS = [
    (re.compile(r"\bcu[A-Z]\w*\b"), "CUDA driver entry point"),
    (re.compile(r"\bcupti[A-Z]\w*\b"), "CUPTI entry point"),
    (re.compile(r"\b(?:ibv|mlx5dv)_\w+\b"), "verbs entry point"),
    (re.compile(r"\bnvshmemx?_\w+\b"), "NVSHMEM entry point"),
    (re.compile(r"\b(?:ucp|uct|ucs)_\w+\b"), "UCX entry point"),
]

# Vendor symbols core is allowed to NAME, per file, each with the reason it is
# not a provider's business. Adding a line here is the deliberate act; the
# check exists so that it cannot happen by accident.
ALLOWED_SYMBOLS = {
    # The symmetric-heap allocator. Core owns object identity (see
    # PLAN-20260810-0505: object tracking is not a provider concern), so these
    # four doors record and close mem objects for whichever providers are
    # loaded -- including none.
    "libpcomm/src/engine.cpp": {
        "nvshmem_malloc",
        "nvshmem_calloc",
        "nvshmem_align",
        "nvshmem_free",
        # One close door for two creation doors. A QP made by ibv_create_qp
        # (ibverbs) and one made by mlx5dv_create_qp (mlx5dv) are both torn
        # down through ibv_destroy_qp, so it can belong to neither provider.
        # engine_abi.h carries the full reasoning.
        "ibv_destroy_qp",
        "ibv_destroy_qp_hook",
    },
}

# An id-range bound is vocabulary, not a symbol: `mlx5dv_lo = 200` names the
# span of record type ids a provider may mint, and ids.hpp is where the
# project's vendor vocabulary is supposed to live. Exempted by shape rather
# than by name so a future range does not have to come back here.
ID_RANGE_BOUND = re.compile(r"_(?:lo|hi)$")


def strip_comments_and_strings(text: str) -> list[str]:
    """Blank out comments and literals, keeping line count and column positions.

    Returns one string per line, same length as the input line, with every
    commented or quoted character replaced by a space. Keeping the geometry is
    what lets a hit report the line number and the surrounding source text the
    reader will actually recognise.

    Hand-written rather than regex'd because the cases that matter are exactly
    the ones a regex gets wrong: a `//` inside a string, a quote inside a
    comment, and `R"(...)"` raw strings (which pcomm-read uses for SQL, and
    which nothing stops libpcomm from growing).
    """
    out = []
    i, n = 0, len(text)
    state = "code"  # code | line_comment | block_comment | string | char | raw
    raw_delim = ""
    buf = []
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state, buf = "line_comment", buf + [" ", " "]
                i += 2
                continue
            if c == "/" and nxt == "*":
                state, buf = "block_comment", buf + [" ", " "]
                i += 2
                continue
            # R"delim( ... )delim" -- the R must not be part of an identifier.
            if c == "R" and nxt == '"' and (not buf or not (buf[-1].isalnum() or buf[-1] == "_")):
                close = text.find("(", i + 2)
                if close != -1 and "\n" not in text[i + 2 : close]:
                    raw_delim = text[i + 2 : close]
                    state = "raw"
                    buf += [" "] * (close - i + 1)
                    i = close + 1
                    continue
            if c == '"':
                state, buf = "string", buf + [" "]
                i += 1
                continue
            if c == "'":
                state, buf = "char", buf + [" "]
                i += 1
                continue
            buf.append(c)
            i += 1
        elif state == "line_comment":
            if c == "\n":
                state = "code"
                buf.append(c)
            else:
                buf.append(" ")
            i += 1
        elif state == "block_comment":
            if c == "*" and nxt == "/":
                state = "code"
                buf += [" ", " "]
                i += 2
                continue
            buf.append(c if c == "\n" else " ")
            i += 1
        elif state in ("string", "char"):
            quote = '"' if state == "string" else "'"
            if c == "\\":
                buf += [" ", " "]
                i += 2
                continue
            if c == quote:
                state = "code"
                buf.append(" ")
                i += 1
                continue
            # An unterminated literal at end of line means the file does not
            # compile; fall back to code so the check does not blank the rest.
            buf.append(c if c == "\n" else " ")
            if c == "\n":
                state = "code"
            i += 1
        elif state == "raw":
            closer = ")" + raw_delim + '"'
            if text.startswith(closer, i):
                state = "code"
                buf += [" "] * len(closer)
                i += len(closer)
                continue
            buf.append(c if c == "\n" else " ")
            i += 1
    return "".join(buf).split("\n")


def tracked_sources(repo: Path) -> list[str]:
    listed = subprocess.run(
        ["git", "ls-files", f"{ROOT_DIR}/"],
        cwd=repo,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    return [
        p
        for p in listed
        if Path(p).suffix in SOURCE_SUFFIXES and not p.startswith(EXCLUDE_DIRS)
    ]


def main() -> int:
    repo = Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    )

    files = tracked_sources(repo)
    if not files:
        print(f"vendor-free: no sources found under {ROOT_DIR}/ -- check is not running", file=sys.stderr)
        return 1

    leaks: list[tuple[str, int, str, str]] = []
    for rel in files:
        raw = (repo / rel).read_text(encoding="utf-8", errors="replace")
        raw_lines = raw.split("\n")
        code_lines = strip_comments_and_strings(raw)
        for lineno, code in enumerate(code_lines, start=1):
            if not code.strip():
                continue
            source = raw_lines[lineno - 1].strip()
            if FORBIDDEN_INCLUDES.match(code):
                leaks.append((rel, lineno, "vendor header", source))
                continue

            hit = next(
                ((m.group(0), what) for pattern, what in VENDOR_TYPES if (m := pattern.search(code))),
                None,
            )
            if hit:
                leaks.append((rel, lineno, f"{hit[1]} `{hit[0]}`", source))
                continue

            allowed = ALLOWED_SYMBOLS.get(rel, frozenset())
            for pattern, what in VENDOR_SYMBOLS:
                names = [
                    name
                    for name in pattern.findall(code)
                    if name not in allowed and not ID_RANGE_BOUND.search(name)
                ]
                if names:
                    leaks.append((rel, lineno, f"{what} `{names[0]}`", source))
                    break

    if leaks:
        print(f"vendor-free: {ROOT_DIR}/ is L0-L2 core and may not name vendor types.", file=sys.stderr)
        print("These are in code, not in comments or string literals:\n", file=sys.stderr)
        for rel, lineno, what, source in leaks:
            print(f"  {rel}:{lineno}: {what}", file=sys.stderr)
            print(f"      {source}", file=sys.stderr)
        print(
            "\n  A TYPE or HEADER belongs in providers/ or gpu-backends/. If core genuinely\n"
            "  needs the ABI, copy it and rename it -- core/mem_attr.hpp declares its own\n"
            "  function-pointer signature, core/dmabuf_registry.hpp writes `using CuResult\n"
            "  = int;` -- and say why in a comment above the declaration.\n"
            "\n"
            "  A SYMBOL core interposes on purpose goes in ALLOWED_SYMBOLS in this script,\n"
            "  with the reason it is not a provider's business. Before adding one, check\n"
            "  that it is not: a door only one provider uses belongs to that provider.",
            file=sys.stderr,
        )
        return 1

    print(f"vendor-free: {len(files)} files under {ROOT_DIR}/ name no vendor type")
    return 0


if __name__ == "__main__":
    sys.exit(main())
