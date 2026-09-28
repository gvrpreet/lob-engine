#pragma once
#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace lob {

inline constexpr std::size_t kHugePageBytes = std::size_t{2} << 20;

// Allocator for the huge-page storage variant (phase 2d): 2 MiB-aligned storage, rounded up to
// whole huge pages, with MADV_HUGEPAGE applied before the container first touches it. The kernel
// honours the hint only when THP is `madvise` or `always`; with `never` (or off Linux) this is a
// plain aligned allocation. A separate type rather than a runtime flag, so the default containers
// keep std::allocator and their exact code generation.
template <class T>
struct HugePageAllocator {
  using value_type = T;

  HugePageAllocator() noexcept = default;
  template <class U>
  HugePageAllocator(const HugePageAllocator<U>&) noexcept {}

  T* allocate(std::size_t n) {
    if (n > (static_cast<std::size_t>(-1) - kHugePageBytes) / sizeof(T)) throw std::bad_array_new_length();
    const std::size_t bytes = (n * sizeof(T) + kHugePageBytes - 1) / kHugePageBytes * kHugePageBytes;
    void* p = std::aligned_alloc(kHugePageBytes, bytes);
    if (p == nullptr) throw std::bad_alloc();
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    (void)madvise(p, bytes, MADV_HUGEPAGE);
#endif
    return static_cast<T*>(p);
  }

  void deallocate(T* p, std::size_t) noexcept { std::free(p); }

  template <class U>
  bool operator==(const HugePageAllocator<U>&) const noexcept { return true; }
};

}  // namespace lob
