#include <cstring>
#include <vector>

#include "arena.hpp"
#include "test_util.hpp"

int main() {
  // --- alignment for every power of two, and no overlap between allocations
  {
    Arena a(1 << 20);
    std::vector<std::pair<unsigned char*, std::size_t>> blocks;
    for (std::size_t align = 1; align <= 4096; align <<= 1) {
      auto* p = static_cast<unsigned char*>(a.alloc(24, align));
      CHECK(p != nullptr);
      CHECK(reinterpret_cast<std::uintptr_t>(p) % align == 0);
      std::memset(p, static_cast<int>(align & 0xFF), 24);
      blocks.push_back({p, 24});
    }
    for (std::size_t i = 0; i < blocks.size(); ++i)
      for (std::size_t j = i + 1; j < blocks.size(); ++j)
        CHECK(blocks[i].first + blocks[i].second <= blocks[j].first);  // disjoint, increasing
  }

  // --- zero internal fragmentation: 1000 x 64B @64 consume exactly 64000 bytes
  {
    Arena a(1 << 20);
    for (int i = 0; i < 1000; ++i) CHECK(a.alloc(64, 64) != nullptr);
    CHECK(a.used() == 64000);
    // odd sizes with alignment 1 are packed back-to-back as well
    a.reset();
    for (int i = 0; i < 100; ++i) CHECK(a.alloc(7, 1) != nullptr);
    CHECK(a.used() == 700);
  }

  // --- exhaustion: exact fit succeeds, one byte more fails, state unchanged
  {
    Arena a(4096);
    const std::size_t cap = a.capacity();
    CHECK(a.alloc(cap + 1, 1) == nullptr);
    CHECK(a.used() == 0);
    CHECK(a.alloc(cap, 1) != nullptr);
    CHECK(a.alloc(1, 1) == nullptr);
    CHECK(a.used() == cap);
    CHECK(a.alloc(~std::size_t{0}, 1) == nullptr);  // size overflow handled
  }

  // --- invalid alignment rejected
  {
    Arena a(4096);
    CHECK(a.alloc(8, 0) == nullptr);
    CHECK(a.alloc(8, 3) == nullptr);
    CHECK(a.alloc(8, 24) == nullptr);
  }

  // --- reset is O(1) and recycles: same first pointer again, memory reusable
  {
    Arena a(1 << 16);
    void* first = a.alloc(128, 64);
    a.alloc(1000, 8);
    CHECK(a.used() > 0);
    a.reset();
    CHECK(a.used() == 0);
    CHECK(a.alloc(128, 64) == first);
  }

  // --- create<T>()
  {
    struct P { int a; double b; };
    Arena a(4096);
    P* p = a.create<P>(P{7, 2.5});
    CHECK(p && p->a == 7 && p->b == 2.5);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % alignof(P) == 0);
  }

  // --- huge-page request must never fail (falls back gracefully)
  {
    Arena a(3 * 1024 * 1024, /*huge_pages=*/true);
    CHECK(a.capacity() % Arena::kHugePageSize == 0);
    void* p = a.alloc(4096, 4096);
    CHECK(p != nullptr);
    std::memset(p, 1, 4096);
    std::printf("  huge-page request granted as: %s\n", to_string(a.page_mode()));
  }

  std::puts("test_arena: all checks passed");
  return 0;
}
