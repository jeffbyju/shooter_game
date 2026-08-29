# Centralised warning / optimisation policy, exported as the `ge_flags` target.
add_library(ge_flags INTERFACE)

target_compile_options(ge_flags INTERFACE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
  -Wunused -Woverloaded-virtual -Wdouble-promotion
  -Wno-gnu-zero-variadic-macro-arguments
  -fno-omit-frame-pointer          # keep stacks walkable under perf/Instruments
)

# -O3 everywhere that is not an explicit Debug build; the hot paths here are
# measured, so the benchmark numbers only mean anything at this level.
target_compile_options(ge_flags INTERFACE $<$<NOT:$<CONFIG:Debug>>:-O3>)

if(GE_NATIVE_ARCH)
  include(CheckCXXCompilerFlag)
  # Acceptance is not correctness: x86 GCC *accepts* -mcpu=native and only warns
  # that it is deprecated, so probing -mcpu first silently hands x86 a tune-only
  # flag and no -march at all. Branch on the target instead of on acceptance.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)")
    check_cxx_compiler_flag("-mcpu=native" GE_HAS_MCPU_NATIVE)
    if(GE_HAS_MCPU_NATIVE)
      target_compile_options(ge_flags INTERFACE -mcpu=native)
    endif()
  else()
    check_cxx_compiler_flag("-march=native" GE_HAS_MARCH_NATIVE)
    if(GE_HAS_MARCH_NATIVE)
      target_compile_options(ge_flags INTERFACE -march=native -mtune=native)
    endif()
  endif()
endif()

if(GE_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(GE_SANITIZE)
  target_compile_options(ge_flags INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all)
  target_link_options(ge_flags    INTERFACE -fsanitize=address,undefined)
endif()
