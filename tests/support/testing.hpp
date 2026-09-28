#pragma once
// Minimal zero-dependency test harness (swap for Catch2/GoogleTest later if you like).
#include <cstdio>
#include <vector>

namespace testing {
struct Case { const char* name; void (*fn)(); };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
struct Registrar { Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); } };
inline int run_all() {
  for (const auto& c : registry()) {
    const int before = failures();
    c.fn();
    std::printf("[%s] %s\n", failures() == before ? " ok " : "FAIL", c.name);
  }
  std::printf("%zu tests, %d failed checks\n", registry().size(), failures());
  return failures() ? 1 : 0;
}
}  // namespace testing

#define TEST(name)                                       \
  static void name();                                    \
  static ::testing::Registrar reg_##name(#name, name);   \
  static void name()

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++::testing::failures();                                                   \
    }                                                                            \
  } while (0)
