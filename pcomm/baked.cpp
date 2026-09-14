// The archive at the end of the launcher: writing it (`pcomm bake`) and
// unpacking it (everything else). Format and rationale: baked.hpp.
#include "baked.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace pcomm::baked {
namespace {

constexpr char        kMagic[8] = {'P', 'C', 'O', 'M', 'M', 'B', 'K', '1'};
constexpr std::size_t kFooter = 8 + 8 + 8 + 4 + 4 + 8;  // see baked.hpp
constexpr unsigned    kFormatVersion = 1;

// FNV-1a. Not a cryptographic claim — it names a cache directory, and the only
// property needed is that different bytes get different names.
unsigned long long fnv(const void* data, std::size_t n, unsigned long long h = 1469598103934665603ull) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Little-endian put/get, spelled out rather than memcpy'd from the host layout,
// so the format stays a format rather than whatever this compiler packed today.
void put64(std::string& out, unsigned long long v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void put32(std::string& out, unsigned v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
unsigned long long get64(const unsigned char* p) {
    unsigned long long v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}
unsigned get32(const unsigned char* p) {
    unsigned v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

bool read_at(int fd, void* buf, std::size_t n, off_t off) {
    auto* p = static_cast<char*>(buf);
    while (n > 0) {
        ssize_t r = ::pread(fd, p, n, off);
        if (r <= 0)
            return false;
        p += r;
        n -= static_cast<std::size_t>(r);
        off += r;
    }
    return true;
}

// Parse the archive at the end of `path`. Returns false when there is none,
// which is not an error: an unbaked launcher is the normal build-tree state.
bool parse(const std::string& path, std::vector<Entry>& out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) < kFooter) {
        ::close(fd);
        return false;
    }
    unsigned char foot[kFooter];
    bool          ok = read_at(fd, foot, kFooter, st.st_size - static_cast<off_t>(kFooter));
    if (!ok || std::memcmp(foot + 32, kMagic, 8) != 0) {
        ::close(fd);
        return false;
    }
    const unsigned long long toc_off = get64(foot + 8);
    const unsigned long long toc_len = get64(foot + 16);
    const unsigned           count = get32(foot + 24);
    const unsigned           version = get32(foot + 28);
    // A newer format in an older binary: say so rather than mis-parse. This is
    // the one case where silence would produce a launcher that quietly cannot
    // find providers it is carrying.
    if (version != kFormatVersion) {
        std::fprintf(stderr, "pcomm: this binary carries a bake archive of format %u; this "
                             "build understands %u\n", version, kFormatVersion);
        ::close(fd);
        return false;
    }
    if (toc_off + toc_len > static_cast<unsigned long long>(st.st_size)) {
        ::close(fd);
        return false;
    }
    std::string toc(toc_len, '\0');
    ok = read_at(fd, toc.data(), toc_len, static_cast<off_t>(toc_off));
    ::close(fd);
    if (!ok)
        return false;

    const auto* p = reinterpret_cast<const unsigned char*>(toc.data());
    const auto* end = p + toc_len;
    for (unsigned i = 0; i < count; ++i) {
        if (static_cast<std::size_t>(end - p) < 4)
            return false;
        unsigned nlen = get32(p);
        p += 4;
        if (static_cast<std::size_t>(end - p) < nlen + 24u)
            return false;
        Entry e;
        e.name.assign(reinterpret_cast<const char*>(p), nlen);
        p += nlen;
        e.offset = get64(p);
        e.size = get64(p + 8);
        e.hash = get64(p + 16);
        p += 24;
        out.push_back(std::move(e));
    }
    return true;
}

std::string self_path() {
    std::error_code ec;
    fs::path        self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : self.string();
}

// One tag for the whole archive, over names, sizes and per-blob hashes. It names
// the cache directory, so a different payload can never be served from a
// previous run's copies — the failure this project has spent four false
// findings on (PLAN-20260904-1900) is not one to re-introduce in a cache.
std::string cache_tag(const std::vector<Entry>& es) {
    unsigned long long h = fnv("pcomm-bake", 10);
    for (const auto& e : es) {
        h = fnv(e.name.data(), e.name.size(), h);
        h = fnv(&e.size, sizeof e.size, h);
        h = fnv(&e.hash, sizeof e.hash, h);
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", h);
    return buf;
}

}  // namespace

const std::vector<Entry>& contents() {
    static const std::vector<Entry> v = [] {
        std::vector<Entry> es;
        const std::string  self = self_path();
        if (!self.empty())
            parse(self, es);
        return es;
    }();
    return v;
}

std::string materialize(const std::string& name) {
    const auto& es = contents();
    const Entry* found = nullptr;
    for (const auto& e : es)
        if (e.name == name)
            found = &e;
    if (!found)
        return {};

    const char* base = std::getenv("PCOMM_BAKE_DIR");
    if (!base || !*base)
        base = std::getenv("TMPDIR");
    if (!base || !*base)
        base = "/tmp";
    const fs::path dir = fs::path(base) / ("pcomm-baked-" + cache_tag(es));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !fs::is_directory(dir)) {
        std::fprintf(stderr, "pcomm: cannot create %s for the baked providers: %s\n",
                     dir.c_str(), ec.message().c_str());
        std::fputs("       set PCOMM_BAKE_DIR to a writable directory\n", stderr);
        return {};
    }

    const fs::path out = dir / name;
    // Already unpacked. The directory name is content-derived, so a file of the
    // right size in it came from this exact archive; anything else would mean
    // someone edited the cache, and re-unpacking would not fix that.
    if (fs::exists(out) && fs::file_size(out, ec) == found->size && !ec)
        return out.string();

    const std::string self = self_path();
    int               in = self.empty() ? -1 : ::open(self.c_str(), O_RDONLY);
    if (in < 0) {
        std::fprintf(stderr, "pcomm: cannot read my own binary to unpack %s\n", name.c_str());
        return {};
    }
    // Written under a unique name and then linked into place, because a node runs
    // many ranks and they all unpack at once: a reader sees either no file or a
    // complete one, never a half-written library being dlopen'd.
    const fs::path tmp = dir / (name + "." + std::to_string(::getpid()) + ".tmp");
    int            o = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (o < 0) {
        std::fprintf(stderr, "pcomm: cannot write %s: %s\n", tmp.c_str(), std::strerror(errno));
        ::close(in);
        return {};
    }
    std::vector<char>  buf(1 << 20);
    unsigned long long left = found->size;
    off_t              off = static_cast<off_t>(found->offset);
    bool               ok = true;
    while (left > 0 && ok) {
        const std::size_t n = static_cast<std::size_t>(left < buf.size() ? left : buf.size());
        ok = read_at(in, buf.data(), n, off);
        if (ok) {
            const char* p = buf.data();
            std::size_t w = n;
            while (w > 0) {
                ssize_t k = ::write(o, p, w);
                if (k <= 0) {
                    ok = false;
                    break;
                }
                p += k;
                w -= static_cast<std::size_t>(k);
            }
        }
        off += static_cast<off_t>(n);
        left -= n;
    }
    ::close(in);
    ::close(o);
    if (!ok) {
        std::fprintf(stderr, "pcomm: failed to unpack %s: %s\n", name.c_str(),
                     std::strerror(errno));
        fs::remove(tmp, ec);
        return {};
    }
    // Placed with link(), NOT rename(). This used to rename, under a comment
    // saying the loser of a race would fail and find an identical file in place.
    // It does not fail: rename(2) replaces an existing name silently, so the
    // second rank's rename unlinked the library the first had already dlopen'd.
    // Nothing was ever torn — both copies came from this archive — but the first
    // rank's /proc/self/maps then said "(deleted)", its provenance could not be
    // proven, and images_are_no_older_than_the_engine turned the board red.
    // Measured on du04 with a fresh bake directory per run: 5 of 8 runs.
    //
    // link() fails with EEXIST instead of replacing, and EEXIST here means
    // another rank placed an identical file first — which is success.
    const int linked = ::link(tmp.c_str(), out.c_str());
    const int link_errno = errno;
    if (linked == 0 || link_errno == EEXIST) {
        fs::remove(tmp, ec);
        return out.string();
    }
    // A filesystem without hard links. rename stays the fallback, and only onto
    // a name still free — narrowing the window, not closing it, which is why it
    // is the fallback and not the method.
    if (!fs::exists(out))
        fs::rename(tmp, out, ec);
    fs::remove(tmp, ec);
    if (!fs::exists(out)) {
        std::fprintf(stderr, "pcomm: cannot place %s: %s\n", out.c_str(),
                     std::strerror(link_errno));
        return {};
    }
    return out.string();
}

namespace {

// FNV-1a over a whole file, the same way bake_main hashes what it writes — so a
// disk copy and a carried copy can be compared without unpacking anything.
bool hash_file(const std::string& path, unsigned long long& out, unsigned long long& size) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::vector<char>  buf(1 << 20);
    unsigned long long h = 1469598103934665603ull;
    std::size_t        n;
    size = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
        h = fnv(buf.data(), n, h);
        size += n;
    }
    std::fclose(f);
    out = h;
    return true;
}

// `pcomm bake --check <binary> <libs...>` — does the binary already carry
// exactly these files, byte for byte?
//
// THIS IS THE REBAKE TRIGGER, and it exists because baking happens at the end
// of a build while `--no-build` runs skip the whole block: a launcher baked
// yesterday would go on carrying yesterday's providers, and nothing on disk
// would look wrong because the disk copies are the ones a build tree actually
// loads. The payload is only consulted when the disk copies are gone — i.e. on
// the machine you copied the single file to, which is the worst place to
// discover it is a day stale.
//
// Compares hashes rather than mtimes on purpose. Every provider here is copied
// between build trees, so mtimes say when the copy happened and nothing about
// what is in it; this is the one place in the tree where the content is cheap
// to hash and therefore worth hashing.
int check_main(const char* host, const std::vector<std::string>& inputs) {
    std::vector<Entry> es;
    parse(host, es);
    int problems = 0;
    for (const auto& in : inputs) {
        const std::string  name = fs::path(in).filename().string();
        unsigned long long h = 0, size = 0;
        if (!hash_file(in, h, size)) {
            std::fprintf(stderr, "pcomm bake --check: cannot read %s\n", in.c_str());
            ++problems;
            continue;
        }
        const Entry* e = nullptr;
        for (const auto& c : es)
            if (c.name == name)
                e = &c;
        if (!e) {
            std::printf("  not carried : %s\n", name.c_str());
            ++problems;
        } else if (e->size != size || e->hash != h) {
            std::printf("  stale       : %s (carries %llu bytes %016llx, disk has %llu %016llx)\n",
                        name.c_str(), e->size, e->hash, size, h);
            ++problems;
        }
    }
    // Carried but not offered: a provider that was dropped from the build would
    // otherwise live on inside the binary, which is the same lie in reverse.
    for (const auto& c : es) {
        bool offered = false;
        for (const auto& in : inputs)
            offered |= fs::path(in).filename().string() == c.name;
        if (!offered) {
            std::printf("  extra       : %s is carried but was not offered\n", c.name.c_str());
            ++problems;
        }
    }
    if (problems == 0) {
        std::printf("%s already carries these %zu file(s)\n", host, inputs.size());
        return 0;
    }
    std::printf("%s: %d difference(s) — rebake\n", host, problems);
    return 1;
}

}  // namespace

int bake_main(int argc, char** argv) {
    const char* out_path = nullptr;
    const char* host = nullptr;
    bool        list_only = false;
    bool        check_only = false;
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if ((a == "-o" || a == "--out") && i + 1 < argc)
            out_path = argv[++i];
        else if (a == "--list")
            list_only = true;
        else if (a == "--check")
            check_only = true;
        else if (a == "-h" || a == "--help") {
            host = nullptr;
            break;
        } else if (!host)
            host = argv[i];
        else
            inputs.emplace_back(argv[i]);
    }

    if (list_only) {
        // `pcomm bake --list [binary]` — what does it carry? Defaults to this
        // binary, which is how you check a launcher someone handed you.
        std::vector<Entry> es;
        std::string        target = host ? host : self_path();
        if (!parse(target, es)) {
            std::printf("%s carries no baked providers\n", target.c_str());
            return 0;
        }
        unsigned long long total = 0;
        std::printf("%s carries %zu file(s):\n", target.c_str(), es.size());
        for (const auto& e : es) {
            std::printf("  %-28s %10llu bytes  %016llx\n", e.name.c_str(), e.size, e.hash);
            total += e.size;
        }
        std::printf("  %zu file(s), %.1f MB\n", es.size(), total / 1e6);
        return 0;
    }

    if (check_only) {
        if (!host || inputs.empty()) {
            std::fputs("usage: pcomm bake --check <binary> <library.so>...\n", stderr);
            return 2;
        }
        return check_main(host, inputs);
    }

    if (!host || inputs.empty() || !out_path) {
        std::fputs(
            "usage: pcomm bake -o <new binary> <host binary> <library.so>...\n"
            "       pcomm bake --list [binary]\n"
            "       pcomm bake --check <binary> <library.so>...   (0 = payload is current)\n"
            "\n"
            "  Copies <host binary> to <new binary> with the libraries appended, so the\n"
            "  result carries its own providers and needs nothing beside it. The launcher\n"
            "  unpacks what a run selects into $PCOMM_BAKE_DIR (default $TMPDIR, /tmp) and\n"
            "  preloads from there.\n"
            "\n"
            "  Providers found on disk still win — PCOMM_PROVIDER_PATH, then the binary's\n"
            "  own directory, then ../lib/pcomm. Baked copies are the fallback, so a baked\n"
            "  launcher inside a build tree behaves exactly as it did before.\n"
            "\n"
            "  Re-baking is exact, not cumulative: the host's original length is recorded,\n"
            "  so baking a baked binary replaces the payload rather than stacking one.\n",
            stderr);
        return 2;
    }

    std::error_code ec;
    // Read the host, dropping any archive it already carries.
    int hf = ::open(host, O_RDONLY);
    if (hf < 0) {
        std::fprintf(stderr, "pcomm bake: cannot read %s: %s\n", host, std::strerror(errno));
        return 1;
    }
    struct stat hst {};
    ::fstat(hf, &hst);
    unsigned long long host_size = static_cast<unsigned long long>(hst.st_size);
    if (static_cast<std::size_t>(hst.st_size) >= kFooter) {
        unsigned char foot[kFooter];
        if (read_at(hf, foot, kFooter, hst.st_size - static_cast<off_t>(kFooter)) &&
            std::memcmp(foot + 32, kMagic, 8) == 0) {
            host_size = get64(foot);
            std::fprintf(stderr, "pcomm bake: %s already carries an archive; replacing it\n", host);
        }
    }

    std::FILE* out = std::fopen(out_path, "wb");
    if (!out) {
        std::fprintf(stderr, "pcomm bake: cannot write %s: %s\n", out_path, std::strerror(errno));
        ::close(hf);
        return 1;
    }
    std::vector<char>  buf(1 << 20);
    unsigned long long left = host_size;
    off_t              off = 0;
    while (left > 0) {
        const std::size_t n = static_cast<std::size_t>(left < buf.size() ? left : buf.size());
        if (!read_at(hf, buf.data(), n, off) || std::fwrite(buf.data(), 1, n, out) != n) {
            std::fprintf(stderr, "pcomm bake: short copy of %s\n", host);
            std::fclose(out);
            ::close(hf);
            return 1;
        }
        off += static_cast<off_t>(n);
        left -= n;
    }
    ::close(hf);

    std::vector<Entry> entries;
    unsigned long long pos = host_size;
    for (const auto& in : inputs) {
        std::FILE* f = std::fopen(in.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "pcomm bake: cannot read %s: %s\n", in.c_str(),
                         std::strerror(errno));
            std::fclose(out);
            return 1;
        }
        Entry e;
        e.name = fs::path(in).filename().string();
        e.offset = pos;
        e.size = 0;
        unsigned long long h = 1469598103934665603ull;
        std::size_t        n;
        while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
            if (std::fwrite(buf.data(), 1, n, out) != n) {
                std::fprintf(stderr, "pcomm bake: short write\n");
                std::fclose(f);
                std::fclose(out);
                return 1;
            }
            h = fnv(buf.data(), n, h);
            e.size += n;
            pos += n;
        }
        std::fclose(f);
        e.hash = h;
        // A duplicate basename would make one of them unreachable — the lookup
        // is by name — so refuse rather than write an archive with a shadowed
        // entry in it.
        for (const auto& prev : entries)
            if (prev.name == e.name) {
                std::fprintf(stderr, "pcomm bake: two inputs are both named %s\n", e.name.c_str());
                std::fclose(out);
                return 1;
            }
        std::printf("  + %-28s %10llu bytes\n", e.name.c_str(), e.size);
        entries.push_back(std::move(e));
    }

    std::string toc;
    for (const auto& e : entries) {
        put32(toc, static_cast<unsigned>(e.name.size()));
        toc.append(e.name);
        put64(toc, e.offset);
        put64(toc, e.size);
        put64(toc, e.hash);
    }
    const unsigned long long toc_off = pos;
    if (std::fwrite(toc.data(), 1, toc.size(), out) != toc.size()) {
        std::fprintf(stderr, "pcomm bake: short write of the table\n");
        std::fclose(out);
        return 1;
    }
    std::string foot;
    put64(foot, host_size);
    put64(foot, toc_off);
    put64(foot, toc.size());
    put32(foot, static_cast<unsigned>(entries.size()));
    put32(foot, kFormatVersion);
    foot.append(kMagic, sizeof kMagic);
    if (std::fwrite(foot.data(), 1, foot.size(), out) != foot.size()) {
        std::fprintf(stderr, "pcomm bake: short write of the footer\n");
        std::fclose(out);
        return 1;
    }
    std::fclose(out);
    // The host's mode, so the result is executable — and so a baked binary keeps
    // whatever the original was, rather than acquiring a mode from the umask.
    fs::permissions(out_path, fs::status(host).permissions(), ec);

    unsigned long long payload = pos - host_size;
    std::printf("pcomm bake: %s -> %s (+%.1f MB, %zu file(s))\n", host, out_path, payload / 1e6,
                entries.size());
    return 0;
}

}  // namespace pcomm::baked
