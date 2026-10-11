cmake_minimum_required(VERSION 3.16)
project(ticker_window CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Werror)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  # Release builds with -O3 on purpose. Set CMAKE_CXX_FLAGS_RELEASE
  # explicitly to "-O3 -DNDEBUG" rather than relying on CMake's default
  # (same string today, but a silent default is easy to override
  # accidentally) and rather than adding a later -O2 via
  # add_compile_options (GCC/Clang take the last -O flag, so a leftover
  # -O2 would win).
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)
endif()

# Deliberately no -Wpedantic: the test oracle (tests/reference_window.hpp)
# uses __int128 (GCC/Clang's widely-supported de facto standard extension)
# for its overflow-free reference sum, which -Wpedantic flags as non-ISO. -Wall -Wextra -Werror stays on
# for everything that indicates a real bug.

# Lifetime/dangling diagnostics, on top of -Wall -Wextra. Compiler-specific
# spellings, and each is probed first: an unrecognized -W flag is only a
# warning to the compiler but -Werror above would make it fatal.
#   Clang: -Wdangling, -Wdangling-gsl (std::string_view/span/optional
#          pointing into a temporary), -Wreturn-stack-address.
#   GCC:   -Wdangling-pointer=2, -Wdangling-reference, -Wreturn-local-addr
#          (the same three families).
include(CheckCXXCompilerFlag)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(_ticker_lifetime_flags -Wdangling -Wdangling-gsl -Wreturn-stack-address)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(_ticker_lifetime_flags -Wdangling-pointer=2 -Wdangling-reference -Wreturn-local-addr)
else()
  set(_ticker_lifetime_flags)
endif()
foreach(_flag ${_ticker_lifetime_flags})
  string(MAKE_C_IDENTIFIER "HAS${_flag}" _var)
  check_cxx_compiler_flag(${_flag} ${_var})
  if(${_var})
    add_compile_options(${_flag})
  endif()
endforeach()

# Sanitizers: TICKER_SANITIZE is a ;-list, e.g. -DTICKER_SANITIZE="address;undefined"
# (or "thread", which cannot be combined with address). Applied to every
# target, compile and link. Use with a Debug or RelWithDebInfo build.
set(TICKER_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. \"address;undefined\" or \"thread\"")
if(TICKER_SANITIZE)
  string(REPLACE ";" "," _ticker_san "${TICKER_SANITIZE}")
  add_compile_options(-fsanitize=${_ticker_san} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${_ticker_san})
  # GCC's -Wtsan flags atomic_thread_fence (used inside Boost.Asio), which
  # -Werror turns fatal; TSan simply doesn't model that fence, and the
  # SPSC rings under test here use per-operation acquire/release instead.
  if(TICKER_SANITIZE MATCHES "thread" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-tsan)
  endif()
endif()

option(TICKER_MARCH_NATIVE "Build with -march=native" OFF)
if(TICKER_MARCH_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" HAS_MARCH_NATIVE)
  if(HAS_MARCH_NATIVE)
    add_compile_options(-march=native)
  endif()
endif()

find_package(Threads REQUIRED)

# --- Boost.Beast / OpenSSL ---------------------------------------------
# On a machine with normal libboost-dev/libssl-dev packages installed,
# these two find_package calls just work and TICKER_SYSROOT is unused.
#
# Without them and without sudo, pass -DTICKER_SYSROOT=<path> to point at
# a local sysroot extracted from the .deb packages instead. Beast/Asio are
# header-only, so all that's actually needed from Boost is the include
# path; OpenSSL still needs its compiled libssl/libcrypto.
set(TICKER_SYSROOT "" CACHE PATH "Optional local sysroot with Boost headers + OpenSSL, for a machine with no dev packages and no sudo")

if(TICKER_SYSROOT)
  set(OPENSSL_ROOT_DIR "${TICKER_SYSROOT}/usr")
  set(OPENSSL_INCLUDE_DIR "${TICKER_SYSROOT}/usr/include")
  set(BOOST_SYSROOT_INCLUDE "${TICKER_SYSROOT}/usr/include")
  # The extracted .deb puts libssl.so/libcrypto.so under the multiarch
  # lib dir, which FindOpenSSL.cmake doesn't probe by default from a
  # nonstandard root -- point it there explicitly.
  file(GLOB _ticker_multiarch_dirs "${TICKER_SYSROOT}/usr/lib/*-linux-gnu")
  list(APPEND CMAKE_LIBRARY_PATH ${_ticker_multiarch_dirs})
  find_library(OPENSSL_SSL_LIBRARY NAMES ssl PATHS ${_ticker_multiarch_dirs} NO_DEFAULT_PATH)
  find_library(OPENSSL_CRYPTO_LIBRARY NAMES crypto PATHS ${_ticker_multiarch_dirs} NO_DEFAULT_PATH)
endif()

find_package(OpenSSL REQUIRED)

if(NOT TICKER_SYSROOT)
  find_package(Boost 1.74 REQUIRED)
endif()

# --- leaf utility library (no networking deps) -----------------------
add_library(ticker_core STATIC
    src/probe.cpp
    src/config.cpp
    src/app_config.cpp
    src/cpu_affinity.cpp
    src/csv_writer.cpp
    src/frame_capture.cpp
)
target_include_directories(ticker_core PUBLIC src)
target_link_libraries(ticker_core PUBLIC Threads::Threads)

# --- networking library (Beast/Asio over TLS) --------------------------
add_library(ticker_net STATIC
    src/ws_feed.cpp
)
target_link_libraries(ticker_net PUBLIC ticker_core OpenSSL::SSL OpenSSL::Crypto)
if(TICKER_SYSROOT)
  target_include_directories(ticker_net SYSTEM PUBLIC "${BOOST_SYSROOT_INCLUDE}")
  # opensslconf.h lives under the multiarch include dir, not usr/include directly.
  file(GLOB _ticker_multiarch_includes "${TICKER_SYSROOT}/usr/include/*-linux-gnu")
  target_include_directories(ticker_net SYSTEM PUBLIC ${_ticker_multiarch_includes})
else()
  target_include_directories(ticker_net SYSTEM PUBLIC ${Boost_INCLUDE_DIRS})
endif()
# Boost.Asio's own resolver internals (ip::basic_resolver_results::create,
# a memcpy into a stack-allocated endpoint) trip both -Wstringop-overflow
# and -Warray-bounds under this GCC 16 trunk build's more aggressive
# fortified-memcpy analysis -- these are false positives in Boost headers,
# not in this project's code (the same pattern is flagged in Boost.Asio's
# issue tracker against recent GCC snapshots). Scoped to this one target
# rather than suppressed project-wide. Clang does not recognize
# -Wno-stringop-overflow, and -Werror would turn that into a hard error.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(ticker_net PRIVATE -Wno-stringop-overflow -Wno-array-bounds)
endif()

add_executable(ticker_feed src/main.cpp)
target_link_libraries(ticker_feed PRIVATE ticker_net)

# --- benchmark ------------------------------------------------------------
add_executable(ticker_bench bench/bench_main.cpp)
target_link_libraries(ticker_bench PRIVATE ticker_core)

# -- compare_policies: WindowPolicy correctness/accuracy check vs exact_map --
# Only needs src/frame_capture.cpp (capture replay) -- no networking, no
# TscTracer/probe machinery (see the file's own header comment for why).
add_executable(compare_policies bench/compare_policies.cpp src/frame_capture.cpp)
target_include_directories(compare_policies PRIVATE src)

# --- unit tests --------------------------------------------------------
enable_testing()

add_executable(ticker_tests
    tests/test_main.cpp
    tests/test_digits.cpp
    tests/test_fixed_price.cpp
    tests/test_iso8601_price.cpp
    tests/test_spsc_ring.cpp
    tests/test_probe.cpp
    tests/test_exact_map_policy.cpp
    tests/test_policy_contract.cpp
    tests/test_sliding_window.cpp
    tests/test_parser.cpp
    tests/test_config.cpp
    tests/test_cpu_affinity.cpp
    tests/test_symbol_registry.cpp
    tests/test_csv_writer.cpp
    tests/test_tracked_histogram_policy.cpp
    tests/test_flat_histogram.cpp
    tests/test_range_bucketing.cpp
    tests/test_range_histogram_policy.cpp
    tests/test_range_histogram_seam.cpp
    tests/test_range_histogram_no_rebuild_policy.cpp
    tests/test_sorted_vector_policies.cpp
    tests/test_hybrid_histogram_policy.cpp
)
target_include_directories(ticker_tests PRIVATE third_party/doctest)
target_link_libraries(ticker_tests PRIVATE ticker_net)
add_test(NAME ticker_tests COMMAND ticker_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
