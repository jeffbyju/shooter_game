#include "ge/net/uring.hpp"

#if defined(GE_OS_LINUX)
  #include <cerrno>
  #include <cstring>
  #include <linux/io_uring.h>
  #include <sys/mman.h>
  #include <sys/syscall.h>
  #include <unistd.h>
#endif

namespace ge::net {

#if defined(GE_OS_LINUX)
namespace {
char g_status[160] = "not probed";

int io_uring_setup_(unsigned entries, io_uring_params* p) {
    return int(::syscall(__NR_io_uring_setup, entries, p));
}

bool probe() {
    io_uring_params p{};
    const int fd = io_uring_setup_(8, &p);
    if (fd < 0) {
        std::snprintf(g_status, sizeof g_status,
                      "unavailable: io_uring_setup: %s", std::strerror(errno));
        return false;
    }
    // IORING_FEAT_NODROP and SINGLE_MMAP tell us roughly how modern the kernel
    // is; both arrived in 5.4-5.5 and their absence means the fast paths below
    // would need compatibility handling that is not worth carrying.
    const bool nodrop      = (p.features & IORING_FEAT_NODROP) != 0;
    const bool single_mmap = (p.features & IORING_FEAT_SINGLE_MMAP) != 0;
    std::snprintf(g_status, sizeof g_status,
                  "available (SQ %u, CQ %u entries%s%s)",
                  p.sq_entries, p.cq_entries,
                  nodrop ? ", NODROP" : "",
                  single_mmap ? ", SINGLE_MMAP" : "");
    ::close(fd);
    return nodrop && single_mmap;
}
} // namespace

bool uring_available() noexcept {
    static const bool ok = probe();
    return ok;
}
const char* uring_status() noexcept { (void)uring_available(); return g_status; }

#else

bool uring_available() noexcept { return false; }
const char* uring_status() noexcept { return "unavailable: io_uring is Linux-only"; }

#endif

} // namespace ge::net
