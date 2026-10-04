// arena.hpp - Phase 1: contiguous arena (bump) allocator backed by mmap().
//
//  * one big virtual-memory region obtained straight from the OS (no malloc/new)
//  * alloc(size, alignment): round the bump pointer up, then advance it
//  * zero internal fragmentation: no per-allocation headers/footers; the only
//    waste is the alignment padding the caller explicitly asked for
//  * reset(): O(1) batch deallocation - just move the bump pointer to 0
//
// NOT thread-safe by design: an arena is owned by one thread (the producer in
// the pipeline). That is what makes alloc() a handful of instructions.
// reset() does not run destructors, so only trivially destructible types may be
// created with create<T>().
#pragma once
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

enum class PageMode {
  Normal,           // ordinary 4 KiB pages
  TransparentHuge,  // madvise(MADV_HUGEPAGE) accepted - kernel may use 2 MiB pages
  ExplicitHuge      // MAP_HUGETLB succeeded - guaranteed 2 MiB pages from hugetlbfs pool
};

inline const char* to_string(PageMode m) noexcept {
  switch (m) {
    case PageMode::Normal: return "normal 4K pages";
    case PageMode::TransparentHuge: return "transparent huge pages (madvise)";
    case PageMode::ExplicitHuge: return "explicit huge pages (MAP_HUGETLB)";
  }
  return "?";
}

class Arena {
 public:
  static constexpr std::size_t kHugePageSize = 2u * 1024 * 1024;

  // capacity     : bytes wanted (rounded up to page / huge-page size)
  // huge_pages   : try to back the pool with 2 MiB pages (bonus task)
  // prefault     : touch every page at construction (MAP_POPULATE) so page
  //                faults don't pollute throughput measurements
  explicit Arena(std::size_t capacity, bool huge_pages = false, bool prefault = false) {
    const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    const std::size_t gran = huge_pages ? kHugePageSize : page;
    capacity_ = (capacity + gran - 1) / gran * gran;

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_POPULATE)
    if (prefault) flags |= MAP_POPULATE;
#else
    (void)prefault;
#endif

    void* p = MAP_FAILED;
#if defined(MAP_HUGETLB)
    if (huge_pages) {
      p = ::mmap(nullptr, capacity_, PROT_READ | PROT_WRITE, flags | MAP_HUGETLB, -1, 0);
      if (p != MAP_FAILED) {
        mode_ = PageMode::ExplicitHuge;
        map_base_ = p;
        map_len_ = capacity_;
      }
    }
#endif
    if (p == MAP_FAILED && huge_pages) {
      // Fallback: over-map by 2 MiB so we can hand out a 2 MiB-aligned region,
      // then ask the kernel for transparent huge pages on it.
      const std::size_t len = capacity_ + kHugePageSize;
      void* raw = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, flags, -1, 0);
      if (raw == MAP_FAILED) throw std::bad_alloc();
      map_base_ = raw;
      map_len_ = len;
      auto aligned = (reinterpret_cast<std::uintptr_t>(raw) + kHugePageSize - 1) &
                     ~(static_cast<std::uintptr_t>(kHugePageSize) - 1);
      p = reinterpret_cast<void*>(aligned);
#if defined(MADV_HUGEPAGE)
      if (::madvise(p, capacity_, MADV_HUGEPAGE) == 0) mode_ = PageMode::TransparentHuge;
#endif
    } else if (p == MAP_FAILED) {
      p = ::mmap(nullptr, capacity_, PROT_READ | PROT_WRITE, flags, -1, 0);
      if (p == MAP_FAILED) throw std::bad_alloc();
      map_base_ = p;
      map_len_ = capacity_;
    }
    base_ = static_cast<std::uint8_t*>(p);
  }

  ~Arena() {
    if (map_base_) ::munmap(map_base_, map_len_);
  }

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Returns nullptr if `alignment` is not a power of two or the arena is full.
  void* alloc(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
    const auto cur = reinterpret_cast<std::uintptr_t>(base_) + offset_;
    const auto aligned = (cur + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    const std::size_t pad = static_cast<std::size_t>(aligned - cur);
    const std::size_t left = capacity_ - offset_;  // offset_ <= capacity_ always
    if (pad > left || size > left - pad) return nullptr;  // overflow-safe bounds check
    offset_ += pad + size;
    return reinterpret_cast<void*>(aligned);
  }

  // Typed helper: placement-new a T inside the arena.
  template <typename T, typename... Args>
  T* create(Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>,
                  "reset() never runs destructors; use trivially destructible types");
    void* mem = alloc(sizeof(T), alignof(T));
    return mem ? ::new (mem) T(std::forward<Args>(args)...) : nullptr;
  }

  // O(1) batch deallocation. Every pointer handed out so far becomes invalid.
  void reset() noexcept { offset_ = 0; }

  std::size_t used() const noexcept { return offset_; }
  std::size_t capacity() const noexcept { return capacity_; }
  PageMode page_mode() const noexcept { return mode_; }

 private:
  std::uint8_t* base_ = nullptr;   // start of usable region
  std::size_t capacity_ = 0;       // usable bytes
  std::size_t offset_ = 0;         // the bump pointer (as an offset)
  void* map_base_ = nullptr;       // what we must munmap
  std::size_t map_len_ = 0;
  PageMode mode_ = PageMode::Normal;
};
