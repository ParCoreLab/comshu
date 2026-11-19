// PROBE — can an IMPORTED cuMem mapping be tied back to the exporter's
// allocation without the library's help?
//
// The question behind nccl_p2p's 1,179,948 unplaceable accesses
// (plans/PLAN-20261119-1200): NCCL shares a peer buffer by cuMemCreate ->
// cuMemExportToShareableHandle(POSIX fd) -> socket -> cuMemImportFromShareable
// Handle -> cuMemMap, and nothing an importer's driver call carries names the
// exporter's buffer. Two candidates are already DEAD and are not retested:
//   - the fd's (st_dev, st_ino): one driver-node inode for every export
//     (MN5 job 44705498, tests/hpc/probes/vmmdoors);
//   - fabric handles: cuMemCreate(FABRIC) -> CUDA_ERROR_NOT_PERMITTED, no IMEX
//     (MN5 job 44709806, tests/hpc/probes/fabricshare).
//
// What is asked here, of each candidate, on BOTH sides of one share:
//   A  cuPointerGetAttribute: BUFFER_ID, MEMORY_BLOCK_ID, P2P_TOKENS,
//      RANGE_START/SIZE, MAPPING_BASE/SIZE, ALLOWED_HANDLE_TYPES
//   B  a dma-buf of the mapped range (cuMemGetHandleForAddressRange): its inode
//      and /proc fdinfo — dma-buf files have per-object inodes, unlike the
//      driver-node fd above
//   C  kcmp(KCMP_FILE) of the exporter's fd against the importer's received
//      fd: the kernel's own "same open file description?" answer
//
// A candidate is an IDENTITY only if, with both processes' allocation counters
// deliberately desynchronised, each of TWO imports equals its own export and
// nothing else (not the other export, not an unshared control). The first
// version compared one import with one export and a control, and job 49225862
// "confirmed" two per-process counters that merely started alike.
//
// Two processes, no MPI: fork before CUDA initialises (a forked CUDA context is
// invalid), pass the fd with SCM_RIGHTS over a socketpair exactly as NCCL's
// proxy does. Parent = exporter on GPU 0, child = importer on GPU 1.
#include <cuda.h>
#include <fcntl.h>
#include <linux/kcmp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const char* g_who = "?";

#define DK(x)                                                                                      \
    do {                                                                                           \
        CUresult r_ = (x);                                                                         \
        if (r_ != CUDA_SUCCESS) {                                                                  \
            const char* n_ = nullptr;                                                              \
            cuGetErrorName(r_, &n_);                                                               \
            std::printf("[ipcid] %s FAIL %s -> %s\n", g_who, #x, n_ ? n_ : "?");                   \
            std::fflush(stdout);                                                                   \
            std::exit(3);                                                                          \
        }                                                                                          \
    } while (0)

// Everything one side learns about one mapping. Sent across the socket so the
// importer can compare field by field.
struct Ids {
    int                ok_buffer_id, ok_block_id, ok_p2p, ok_range, ok_mapping, ok_handles, ok_dmabuf;
    unsigned long long buffer_id, block_id, p2p_token, va_token;
    unsigned long long range_start, range_size, mapping_base, mapping_size, handle_types;
    unsigned long long dmabuf_dev, dmabuf_ino;
    char               dmabuf_fdinfo[256];
};

template<typename T> static int attr(T* out, CUpointer_attribute a, CUdeviceptr p) {
    return cuPointerGetAttribute(out, a, p) == CUDA_SUCCESS;
}

static void read_fdinfo(int fd, char* out, size_t n) {
    out[0] = 0;
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
    FILE* f = std::fopen(path, "r");
    if (!f)
        return;
    size_t got = std::fread(out, 1, n - 1, f);
    out[got] = 0;
    std::fclose(f);
    for (char* c = out; *c; ++c)
        if (*c == '\n')
            *c = ' ';
}

static Ids query(CUdeviceptr p) {
    Ids i{};
    i.ok_buffer_id = attr(&i.buffer_id, CU_POINTER_ATTRIBUTE_BUFFER_ID, p);
    i.ok_block_id = attr(&i.block_id, CU_POINTER_ATTRIBUTE_MEMORY_BLOCK_ID, p);
    CUDA_POINTER_ATTRIBUTE_P2P_TOKENS tok{};
    i.ok_p2p = attr(&tok, CU_POINTER_ATTRIBUTE_P2P_TOKENS, p);
    i.p2p_token = tok.p2pToken;
    i.va_token = tok.vaSpaceToken;
    CUdeviceptr rs = 0, mb = 0;
    size_t      rz = 0, mz = 0;
    i.ok_range = attr(&rs, CU_POINTER_ATTRIBUTE_RANGE_START_ADDR, p) &&
                 attr(&rz, CU_POINTER_ATTRIBUTE_RANGE_SIZE, p);
    i.range_start = rs;
    i.range_size = rz;
    i.ok_mapping = attr(&mb, CU_POINTER_ATTRIBUTE_MAPPING_BASE_ADDR, p) &&
                   attr(&mz, CU_POINTER_ATTRIBUTE_MAPPING_SIZE, p);
    i.mapping_base = mb;
    i.mapping_size = mz;
    i.ok_handles = attr(&i.handle_types, CU_POINTER_ATTRIBUTE_ALLOWED_HANDLE_TYPES, p);
    return i;
}

static void dmabuf_of(CUdeviceptr p, size_t size, Ids& i) {
    int fd = -1;
    CUresult r = cuMemGetHandleForAddressRange(&fd, p, size, CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
    if (r != CUDA_SUCCESS || fd < 0) {
        const char* n = nullptr;
        cuGetErrorName(r, &n);
        std::printf("[ipcid] %s dmabuf: cuMemGetHandleForAddressRange -> %s\n", g_who, n ? n : "?");
        return;
    }
    struct stat st{};
    if (fstat(fd, &st) == 0) {
        i.ok_dmabuf = 1;
        i.dmabuf_dev = st.st_dev;
        i.dmabuf_ino = st.st_ino;
    }
    read_fdinfo(fd, i.dmabuf_fdinfo, sizeof i.dmabuf_fdinfo);
    // Deliberately NOT closed: closing could let the next dma-buf reuse the
    // inode, which would make "different" ambiguous within one process.
}

static void print(const char* what, const Ids& i) {
    std::printf("[ipcid] %s %s buffer_id=%s%llu block_id=%s%llu p2p=%s0x%llx/0x%llx "
                "range=%s0x%llx+%llu mapping=%s0x%llx+%llu handle_types=%s0x%llx "
                "dmabuf=%s%llu:%llu fdinfo='%s'\n",
                g_who, what, i.ok_buffer_id ? "" : "FAIL:", i.buffer_id,
                i.ok_block_id ? "" : "FAIL:", i.block_id, i.ok_p2p ? "" : "FAIL:", i.p2p_token,
                i.va_token, i.ok_range ? "" : "FAIL:", i.range_start, i.range_size,
                i.ok_mapping ? "" : "FAIL:", i.mapping_base, i.mapping_size,
                i.ok_handles ? "" : "FAIL:", i.handle_types, i.ok_dmabuf ? "" : "FAIL:",
                i.dmabuf_dev, i.dmabuf_ino, i.dmabuf_fdinfo);
}

static void send_fd(int sock, int fd) {
    char            dummy = 'x';
    struct iovec    io{&dummy, 1};
    char            ctl[CMSG_SPACE(sizeof(int))] = {};
    struct msghdr   m{};
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = ctl;
    m.msg_controllen = sizeof ctl;
    struct cmsghdr* c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
    if (sendmsg(sock, &m, 0) != 1) {
        std::perror("sendmsg");
        std::exit(4);
    }
}

static int recv_fd(int sock) {
    char            dummy;
    struct iovec    io{&dummy, 1};
    char            ctl[CMSG_SPACE(sizeof(int))] = {};
    struct msghdr   m{};
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = ctl;
    m.msg_controllen = sizeof ctl;
    if (recvmsg(sock, &m, 0) != 1) {
        std::perror("recvmsg");
        std::exit(4);
    }
    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(CMSG_FIRSTHDR(&m)), sizeof(int));
    return fd;
}

static void xfer(int sock, void* p, size_t n, bool out) {
    char* c = static_cast<char*>(p);
    while (n) {
        ssize_t k = out ? write(sock, c, n) : read(sock, c, n);
        if (k <= 0) {
            std::perror(out ? "write" : "read");
            std::exit(4);
        }
        c += k;
        n -= static_cast<size_t>(k);
    }
}

// One exportable allocation on `dev`, mapped and accessible there.
static CUdeviceptr make(int dev, size_t size, CUmemGenericAllocationHandle* h) {
    CUmemAllocationProp prop = {};
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = dev;
    prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
    DK(cuMemCreate(h, size, &prop, 0));
    CUdeviceptr va = 0;
    DK(cuMemAddressReserve(&va, size, 0, 0, 0));
    DK(cuMemMap(va, size, 0, *h, 0));
    CUmemAccessDesc acc = {};
    acc.location = prop.location;
    acc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    DK(cuMemSetAccess(va, size, &acc, 1));
    return va;
}

static size_t granule(int dev) {
    CUmemAllocationProp prop = {};
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = dev;
    prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
    size_t g = 0;
    DK(cuMemGetAllocationGranularity(&g, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM));
    return g;
}

int main() {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::perror("socketpair");
        return 2;
    }
    const pid_t child = fork();
    if (child < 0) {
        std::perror("fork");
        return 2;
    }

    if (child > 0) {  // ---------------- EXPORTER, GPU 0 ----------------
        g_who = "exporter";
        close(sv[1]);
        const int s = sv[0];
        DK(cuInit(0));
        CUdevice d;
        DK(cuDeviceGet(&d, 0));
        CUcontext ctx;
        DK(cuDevicePrimaryCtxRetain(&ctx, d));
        DK(cuCtxSetCurrent(ctx));
        const size_t g = granule(0);
        // DESYNCHRONISE THE COUNTERS FIRST. Job 49225862 reported buffer_id and
        // memory_block as identities, but the exporter's X was its first VMM
        // allocation and the importer's import was ITS first: two per-process
        // counters that start alike would agree just the same. So the exporter
        // makes 7 plain allocations and the control Y before X1, and 4 more
        // before X2; the importer makes 3 of its own first and imports the two
        // in REVERSE order with 2 more between. A per-process counter cannot
        // survive that; an allocation identity is untouched by it.
        for (int i = 0; i < 7; ++i) {
            CUdeviceptr junk = 0;
            DK(cuMemAlloc(&junk, 1 << 20));
        }
        CUmemGenericAllocationHandle hy, h1, h2;
        CUdeviceptr y = make(0, 2 * g, &hy);
        CUdeviceptr x1 = make(0, 2 * g, &h1);
        for (int i = 0; i < 4; ++i) {
            CUdeviceptr junk = 0;
            DK(cuMemAlloc(&junk, 1 << 20));
        }
        CUdeviceptr x2 = make(0, 2 * g, &h2);
        // Queried at an INTERIOR address, so a base-only answer would show.
        Ids iy = query(y + g + 64), i1 = query(x1 + g + 64), i2 = query(x2 + g + 64);
        dmabuf_of(y, 2 * g, iy);
        dmabuf_of(x1, 2 * g, i1);
        dmabuf_of(x2, 2 * g, i2);
        print("Y(control)", iy);
        print("X1(shared)", i1);
        print("X2(shared)", i2);

        int fd = -1, fd2 = -1;
        DK(cuMemExportToShareableHandle(&fd, h1, CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR, 0));
        DK(cuMemExportToShareableHandle(&fd2, h2, CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR, 0));
        std::printf("[ipcid] exporter pid=%d export_fd=%d,%d granule=%zu\n", getpid(), fd, fd2, g);
        unsigned long long sz = 2 * g;
        xfer(s, &sz, sizeof sz, true);
        send_fd(s, fd);
        send_fd(s, fd2);
        xfer(s, &i1, sizeof i1, true);
        xfer(s, &i2, sizeof i2, true);
        xfer(s, &iy, sizeof iy, true);

        // kcmp from the PARENT: Yama's ptrace_scope=1 allows a parent to
        // inspect its child, not the reverse.
        int bfd = -1;
        xfer(s, &bfd, sizeof bfd, false);
        long same = syscall(SYS_kcmp, getpid(), child, KCMP_FILE, fd, bfd);
        if (same < 0)
            std::printf("[ipcid] VERDICT %-14s UNAVAILABLE (kcmp: %s)\n", "kcmp_file",
                        std::strerror(errno));
        else
            std::printf("[ipcid] VERDICT %-14s %s (kcmp=%ld)\n", "kcmp_file",
                        same == 0 ? "SAME open file (identity, but needs both pids live)"
                                  : "different open files", same);
        char done = 0;
        xfer(s, &done, 1, true);  // release the child
        int status = 0;
        waitpid(child, &status, 0);
        std::printf("[ipcid] importer exit=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return 0;
    }

    // ---------------- IMPORTER, GPU 1 ----------------
    g_who = "importer";
    close(sv[0]);
    const int s = sv[1];
    DK(cuInit(0));
    int ndev = 0;
    DK(cuDeviceGetCount(&ndev));
    const int mydev = ndev > 1 ? 1 : 0;
    CUdevice d;
    DK(cuDeviceGet(&d, mydev));
    CUcontext ctx;
    DK(cuDevicePrimaryCtxRetain(&ctx, d));
    DK(cuCtxSetCurrent(ctx));

    unsigned long long sz = 0;
    xfer(s, &sz, sizeof sz, false);
    const int fd = recv_fd(s);
    const int fd2 = recv_fd(s);
    Ids e1{}, e2{}, ey{};
    xfer(s, &e1, sizeof e1, false);
    xfer(s, &e2, sizeof e2, false);
    xfer(s, &ey, sizeof ey, false);
    const size_t g = sz / 2;

    // Three allocations of the importer's own before anything is imported.
    Ids mine[3];
    for (int i = 0; i < 3; ++i) {
        CUmemGenericAllocationHandle hl;
        CUdeviceptr                  l = make(mydev, sz, &hl);
        mine[i] = query(l + g + 64);
        print("local", mine[i]);
    }

    auto import = [&](int f) {
        CUmemGenericAllocationHandle h;
        DK(cuMemImportFromShareableHandle(&h, reinterpret_cast<void*>(static_cast<intptr_t>(f)),
                                          CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR));
        CUdeviceptr va = 0;
        DK(cuMemAddressReserve(&va, sz, 0, 0, 0));
        DK(cuMemMap(va, sz, 0, h, 0));
        CUmemAccessDesc acc = {};
        acc.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
        acc.location.id = mydev;
        acc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
        DK(cuMemSetAccess(va, sz, &acc, 1));
        Ids r = query(va + g + 64);
        dmabuf_of(va, sz, r);
        return r;
    };
    // Reverse order, with two more local allocations between.
    Ids b2 = import(fd2);
    for (int i = 0; i < 2; ++i) {
        CUdeviceptr junk = 0;
        DK(cuMemAlloc(&junk, 1 << 20));
    }
    Ids b1 = import(fd);
    std::printf("[ipcid] importer pid=%d dev=%d received_fd=%d,%d\n", getpid(), mydev, fd, fd2);
    print("X1(imported)", b1);
    print("X2(imported)", b2);

    // IDENTITY needs all of: each import equals ITS export, differs from the
    // other export and from the control, and the two imports differ.
    auto judge = [&](const char* name, auto get, auto ok) {
        const bool all_ok = ok(b1) && ok(b2) && ok(e1) && ok(e2) && ok(ey);
        const char* v;
        if (!all_ok)
            v = "UNAVAILABLE (query failed)";
        else if (get(b1) == get(e1) && get(b2) == get(e2) && get(e1) != get(e2) &&
                 get(b1) != get(ey) && get(b2) != get(ey))
            v = "IDENTITY (each import == its export; exports, control all distinct)";
        else if (get(e1) == get(e2) && get(e1) == get(ey))
            v = "useless (same for every allocation)";
        else
            v = "NOT an identity (per-process, or matches the wrong allocation)";
        std::printf("[ipcid] VERDICT %-14s %s\n", name, v);
    };
    judge("buffer_id", [](const Ids& i) { return i.buffer_id; },
          [](const Ids& i) { return i.ok_buffer_id != 0; });
    judge("memory_block", [](const Ids& i) { return i.block_id; },
          [](const Ids& i) { return i.ok_block_id != 0; });
    judge("p2p_token", [](const Ids& i) { return i.p2p_token; },
          [](const Ids& i) { return i.ok_p2p != 0; });
    judge("dmabuf_inode", [](const Ids& i) { return i.dmabuf_ino; },
          [](const Ids& i) { return i.ok_dmabuf != 0; });
    std::fflush(stdout);
    int fd_out = fd;
    xfer(s, &fd_out, sizeof fd_out, true);
    char done;
    xfer(s, &done, 1, false);
    return 0;
}
