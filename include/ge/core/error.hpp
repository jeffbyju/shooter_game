#pragma once
// Error reporting for paths that can fail.
//
// Before C++23 the usual shapes are a bool plus an out-parameter, or an
// exception. The out-parameter form compiles to good code but is easy to misuse:
// nothing stops a caller ignoring the bool, and the success value and the error
// live in different places, so "did it work" and "what do I do with it" are two
// separate questions.
//
// std::expected<T, E> makes the answer one value. A caller cannot reach the
// result without acknowledging the failure case, and the error travels with it.
//
// Note where this is used: startup, configuration and connection paths only.
// Error objects hold a std::string and therefore allocate, which is exactly why
// they are kept off the steady-state path, where failures are counted in
// preallocated counters instead (see metrics/counters.hpp).
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace ge::core {

struct Error {
    std::string message;

    Error() = default;
    explicit Error(std::string m) : message(std::move(m)) {}
    // Builds "context: detail", the shape every errno-based failure wants.
    Error(std::string_view context, std::string_view detail)
        : message(std::string(context) + ": " + std::string(detail)) {}

    const char* c_str() const noexcept { return message.c_str(); }
};

// The two shapes used throughout: a value or an error, and "it worked" or an
// error. Result<void> is spelled Status for readability at call sites.
template <typename T>
using Result = std::expected<T, Error>;

using Status = std::expected<void, Error>;

// Reads like `return fail("bind", std::strerror(errno));`
inline std::unexpected<Error> fail(std::string_view context, std::string_view detail) {
    return std::unexpected(Error{context, detail});
}
inline std::unexpected<Error> fail(std::string_view message) {
    return std::unexpected(Error{std::string(message)});
}

// Succeeds for Status. Spelled out because `return {};` on an expected<void, E>
// is correct but reads like a mistake.
inline Status ok() noexcept { return Status{}; }

} // namespace ge::core
