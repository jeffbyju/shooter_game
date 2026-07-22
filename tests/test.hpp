#pragma once
// Minimal test harness. No external dependency, because a benchmark repo that
// cannot be built without fetching half the internet does not get built.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ge::test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}
inline bool add(const char* name, void (*fn)()) {
    registry().push_back({name, fn});
    return true;
}

inline int  g_failures = 0;
inline int  g_checks   = 0;
inline const char* g_current = "";

inline void fail(const char* file, int line, const std::string& msg) {
    ++g_failures;
    std::printf("    \033[31mFAIL\033[0m %s:%d  %s\n", file, line, msg.c_str());
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    ++g_checks;
    if (!(a == b)) {
        char buf[512];
        std::snprintf(buf, sizeof buf, "expected %s == %s", ea, eb);
        fail(file, line, buf);
    }
}

int run_all(int argc, char** argv);

} // namespace ge::test

#define GE_TEST(name)                                                          \
    static void name();                                                        \
    static const bool name##_registered = ::ge::test::add(#name, name);        \
    static void name()

#define CHECK(expr)                                                            \
    do { ++::ge::test::g_checks;                                               \
         if (!(expr)) ::ge::test::fail(__FILE__, __LINE__, "expected " #expr); \
    } while (0)

#define CHECK_EQ(a, b) ::ge::test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_NEAR(a, b, tol)                                                  \
    do { ++::ge::test::g_checks;                                               \
         const double da_ = double(a), db_ = double(b);                        \
         if (!(da_ >= db_ - (tol) && da_ <= db_ + (tol))) {                    \
             char buf_[256];                                                   \
             std::snprintf(buf_, sizeof buf_, "expected %s (%.6g) within %g of %s (%.6g)", \
                           #a, da_, double(tol), #b, db_);                     \
             ::ge::test::fail(__FILE__, __LINE__, buf_);                       \
         } } while (0)
