#include "test.hpp"

#include <cstring>

namespace ge::test {

int run_all(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (const Case& c : registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        g_current = c.name;
        const int before = g_failures;
        std::printf("  %-44s", c.name);
        std::fflush(stdout);
        c.fn();
        std::printf("%s\n", g_failures == before ? " \033[32mok\033[0m" : "");
        ++ran;
    }
    std::printf("\n%d tests, %d checks, \033[%sm%d failures\033[0m\n",
                ran, g_checks, g_failures ? "31" : "32", g_failures);
    return g_failures == 0 ? 0 : 1;
}

} // namespace ge::test

int main(int argc, char** argv) {
    std::printf("\nge_server unit tests\n--------------------------------------------------\n");
    return ge::test::run_all(argc, argv);
}
