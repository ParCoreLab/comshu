# Copyright ParCore Lab. See the LICENSE file at the root of the pcomm
# repository for details.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

from spack_repo.builtin.build_systems.cmake import CMakePackage

from spack.package import *


class Pcomm(CMakePackage):
    """Captures GPU and network communication from a running workload and reads
    it back as tables: InfiniBand verbs, IBGDA/DEVX, UCX, NCCL collectives and
    CUDA kernel timing."""

    homepage = "https://github.com/ParCoreLab/pcomm"
    git = "https://github.com/ParCoreLab/pcomm.git"

    license("AGPL-3.0-or-later")

    version("main", branch="main")

    # Every provider is off by default, and that matches the CMake defaults
    # rather than being a packaging opinion: the core builds on a host with no
    # CUDA and no verbs, which is what lets it be installed on a login node.
    variant("ibverbs", default=False, description="InfiniBand verbs provider (host-side QPs and sends)")
    variant("mlx5dv", default=False, description="IBGDA/DEVX provider (GPU-initiated InfiniBand)")
    variant("ucx", default=False, description="UCX providers, both the UCP and the UCT layer")
    variant("nccl", default=False, description="NCCL collective provider (host API)")
    variant("cupti", default=False, description="CUPTI kernel-timing backend")
    variant("duckdb", default=False, description="Parquet export and SQL over a capture")
    variant("ui", default=False, description="The `pcomm ui` server, panels in a browser")
    variant("tests", default=True, description="Build the core tests")

    # Serving a capture is querying it, and the root CMakeLists refuses the
    # combination outright rather than quietly building half a UI.
    requires("+duckdb", when="+ui", msg="+ui needs +duckdb: `pcomm ui` queries the capture")

    depends_on("cxx", type="build")
    depends_on("cmake@3.20:", type="build")
    # Record tables and the schema check are generated at build time.
    depends_on("python@3:", type="build")

    # libibverbs for the verbs provider; the same package also carries libmlx5
    # and infiniband/mlx5dv.h, which is what the DEVX provider links.
    depends_on("rdma-core", when="+ibverbs")
    depends_on("rdma-core", when="+mlx5dv")
    depends_on("ucx", when="+ucx")
    depends_on("nccl", when="+nccl")
    # The NCCL provider links no NCCL (it goes through the PROFAPI symbols the
    # application already loaded) but it does include cuda_runtime_api.h.
    depends_on("cuda", when="+nccl")
    depends_on("cuda", when="+cupti")
    # ~python because pcomm uses DuckDB's C API and nothing else: pcomm-read
    # links libduckdb.so against duckdb.h, and never imports the bindings.
    # Measured, not assumed — it removes every py-* node from the spec, which is
    # a whole CPython/setuptools subtree built for a library we link as C.
    #
    # Not constrained here, but worth knowing when +duckdb is slow to install:
    # DuckDB's `icu` variant means the BUNDLED ICU, so the default ~icu pulls
    # the external icu4c, which build-depends on autoconf/automake/libtool. On a
    # machine with no autotools that is a from-source automake before any of
    # pcomm is touched. `^duckdb+icu` drops icu4c and that whole chain.
    depends_on("duckdb~python", when="+duckdb")
    # The UI server is an HTTP server; cpp-httplib is the single header it uses.
    depends_on("cpp-httplib", when="+ui")
    # The other half of the UI is the frontend bundle, which the build now
    # builds (tools/build_ui.sh) rather than carrying in git. That needs node
    # and npm, and npm is its own package here because spack's node-js is
    # configured --without-npm. npm rather than pnpm because spack has no pnpm.
    #
    # This is the one variant whose BUILD needs network: npm fetches the
    # frontend packages. Fine where the UI is wanted, which is a workstation
    # reading a capture; not fine on an offline cluster, which is why +ui is off
    # by default and MN5 builds without it.
    depends_on("node-js", type="build", when="+ui")
    depends_on("npm", type="build", when="+ui")

    # The gpumem provider (NVBit, GPU-initiated NVLink load/store) is absent on
    # purpose. It has to be configured in its own build tree against the NVHPC
    # toolchain with an NVBit-mandated gcc-11.4 host compiler, and it needs
    # nvdisasm on PATH at capture time. That is two toolchains in one spec, so
    # it is built from source instead. See the repository README.

    def setup_build_environment(self, env):
        # gpu-backends/cupti finds cupti.h under $CUDA_HOME/extras/CUPTI, and
        # the NCCL provider finds cuda_runtime_api.h the same way. Spack puts
        # the prefix on CMAKE_PREFIX_PATH, but neither header sits at the top of
        # it, so the hint the CMake actually reads is set here.
        if self.spec.satisfies("+cupti") or self.spec.satisfies("+nccl"):
            env.set("CUDA_HOME", self.spec["cuda"].prefix)

    @run_before("cmake")
    def stage_duckdb(self):
        """Put DuckDB where the build looks for it.

        pcomm-read expects one flat directory, .tools/duckdb, holding duckdb.h
        beside libduckdb.so, because that is what tools/get_duckdb.sh leaves on
        a machine with network access. Spack installs those two files into
        include/ and lib/, so they are linked into the expected shape rather
        than the build being patched to look somewhere else.
        """
        if self.spec.satisfies("~duckdb"):
            return

        prefix = self.spec["duckdb"].prefix
        headers = find(prefix, "duckdb.h", recursive=True)
        libraries = find(prefix, "libduckdb.so", recursive=True)
        if not headers or not libraries:
            raise InstallError(
                "the duckdb dependency has no duckdb.h and libduckdb.so pair under "
                "{0}; pcomm +duckdb needs the C API".format(prefix)
            )

        staged = join_path(self.stage.source_path, ".tools", "duckdb")
        mkdirp(staged)
        force_symlink(headers[0], join_path(staged, "duckdb.h"))
        force_symlink(libraries[0], join_path(staged, "libduckdb.so"))

    @run_before("cmake")
    def stage_httplib(self):
        """Put cpp-httplib where the UI build looks for it.

        Same bargain as stage_duckdb: pcomm-ui wants .tools/httplib/httplib.h,
        because that is where tools/get_httplib.sh drops it on a machine with
        network. Found by search rather than by assuming include/httplib.h,
        since a single-header package is free to nest it.

        The other half of the UI, the frontend bundle, needs no staging either:
        the build builds it with npm (tools/build_ui.sh).
        """
        if self.spec.satisfies("~ui"):
            return

        prefix = self.spec["cpp-httplib"].prefix
        headers = find(prefix, "httplib.h", recursive=True)
        if not headers:
            raise InstallError(
                "the cpp-httplib dependency has no httplib.h under {0}".format(prefix)
            )

        staged = join_path(self.stage.source_path, ".tools", "httplib")
        mkdirp(staged)
        force_symlink(headers[0], join_path(staged, "httplib.h"))

    def cmake_args(self):
        return [
            self.define_from_variant("PCOMM_PROVIDER_IBVERBS", "ibverbs"),
            self.define_from_variant("PCOMM_PROVIDER_MLX5DV", "mlx5dv"),
            self.define_from_variant("PCOMM_PROVIDER_UCX", "ucx"),
            self.define_from_variant("PCOMM_PROVIDER_NCCL", "nccl"),
            self.define_from_variant("PCOMM_CUPTI", "cupti"),
            self.define_from_variant("PCOMM_DUCKDB", "duckdb"),
            self.define_from_variant("PCOMM_BUILD_TESTS", "tests"),
            # Explicit in both directions. PCOMM_UI otherwise FOLLOWS
            # PCOMM_DUCKDB, so every +duckdb spec would try to build the UI and
            # fail on the missing httplib; and a spec that does want it has to
            # say so. `requires` above ties +ui to +duckdb.
            self.define_from_variant("PCOMM_UI", "ui"),
            # Spack already guarantees the right libstdc++ through the
            # gcc-runtime package and RPATH. Carrying a second copy inside every
            # binary would defeat that, and it is only on upstream because the
            # cluster installs have no such guarantee.
            self.define("PCOMM_STATIC_CXXLIB", False),
        ]
