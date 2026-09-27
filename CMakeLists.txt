cmake_minimum_required(VERSION 3.15)
project(FTP VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Threads
# ---------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# OpenSSL (used for TLS and the SHA-256 integrity check)
#
# FindOpenSSL is MSVC centric: on Windows it only looks for import/static .lib
# archives, so a MinGW installation of OpenSSL (libssl.a / libcrypto.a) is
# missed. Fall back to a plain header/library search in that case.
# ---------------------------------------------------------------------------
find_package(OpenSSL QUIET)

if(NOT OpenSSL_FOUND)
    set(_openssl_hints "")
    foreach(root "$ENV{OPENSSL_ROOT_DIR}" "${OPENSSL_ROOT_DIR}")
        if(root)
            list(APPEND _openssl_hints "${root}" "${root}/include" "${root}/lib")
        endif()
    endforeach()

    find_path(OPENSSL_INCLUDE_DIR openssl/ssl.h HINTS ${_openssl_hints})
    find_library(OPENSSL_SSL_LIBRARY NAMES ssl libssl ssl_3 libssl_3 HINTS ${_openssl_hints})
    find_library(OPENSSL_CRYPTO_LIBRARY NAMES crypto libcrypto crypto_3 libcrypto_3 HINTS ${_openssl_hints})

    if(OPENSSL_INCLUDE_DIR AND OPENSSL_SSL_LIBRARY AND OPENSSL_CRYPTO_LIBRARY)
        add_library(OpenSSL::SSL UNKNOWN IMPORTED)
        set_target_properties(OpenSSL::SSL PROPERTIES
            IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
        )
        add_library(OpenSSL::Crypto UNKNOWN IMPORTED)
        set_target_properties(OpenSSL::Crypto PROPERTIES
            IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
        )
        message(STATUS "OpenSSL (manual): ${OPENSSL_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR
            "OpenSSL was not found. Install OpenSSL development libraries or pass "
            "-DOPENSSL_ROOT_DIR=<prefix>.")
    endif()
else()
    message(STATUS "OpenSSL: ${OPENSSL_INCLUDE_DIR} (${OPENSSL_VERSION})")
endif()

# ---------------------------------------------------------------------------
# Boost.Asio
#
# Boost.Asio is header only, so only the headers are needed and no Boost
# libraries have to be linked. They are looked for in this order:
#   1. an explicit -DBOOST_ROOT=<path>
#   2. a copy vendored in include/boost
#   3. the system installation
#
# find_path is used instead of find_package(Boost) because the FindBoost module
# is deprecated (policy CMP0167) and scheduled for removal.
# ---------------------------------------------------------------------------
if(DEFINED BOOST_ROOT)
    set(BOOST_ROOT_HINT "${BOOST_ROOT}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/asio.hpp")
    set(BOOST_ROOT_HINT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

find_path(BOOST_ASIO_INCLUDE_DIR
    NAMES boost/asio.hpp
    HINTS ${BOOST_ROOT_HINT} "$ENV{BOOST_ROOT}"
    PATH_SUFFIXES include
)
if(NOT BOOST_ASIO_INCLUDE_DIR)
    message(FATAL_ERROR
        "Boost headers not found. Install Boost >= 1.70 or pass -DBOOST_ROOT=<path> "
        "pointing at the directory that contains boost/asio.hpp.")
endif()
message(STATUS "Boost headers: ${BOOST_ASIO_INCLUDE_DIR}")

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(SRC
    src/main.cpp
    src/server.cpp
    src/client.cpp
    src/serverSession.cpp
    src/checksum.cpp
)

add_executable(FTP ${SRC})

target_include_directories(FTP PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${BOOST_ASIO_INCLUDE_DIR}
)

if(MSVC)
    target_compile_options(FTP PRIVATE /W4 /permissive-)
    target_compile_definitions(FTP PRIVATE
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
else()
    target_compile_options(FTP PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# TLS assets: copy the server certificate/key next to the executable so the
# server can find them relative to its working directory. Optional, so that a
# missing tls_key/ directory does not break the configure step.
# ---------------------------------------------------------------------------
set(TLS_KEY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tls_key")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tls_key")
foreach(asset server.crt server.key)
    if(EXISTS "${TLS_KEY_DIR}/${asset}")
        file(COPY "${TLS_KEY_DIR}/${asset}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/tls_key")
        file(COPY "${TLS_KEY_DIR}/${asset}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
        message(STATUS "TLS asset found: ${TLS_KEY_DIR}/${asset}")
    else()
        message(WARNING "TLS asset not found: ${TLS_KEY_DIR}/${asset} - "
                        "generate it with: openssl req -x509 -newkey rsa:2048 -nodes "
                        "-keyout tls_key/server.key -out tls_key/server.crt -days 365 "
                        "-subj \"/CN=localhost\"")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------
target_link_libraries(FTP
    PRIVATE
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)

if(WIN32)
    # ws2_32: sockets, mswsock: AcceptEx/GetAcceptExSockaddrs used by the acceptor.
    target_link_libraries(FTP PRIVATE ws2_32 mswsock)
endif()

# ---------------------------------------------------------------------------
# Tests
#
# tests/run_tests.ps1 starts a real server and drives real clients against it.
# It is registered with CTest so `ctest` runs it, but it is not part of the
# default build target.
# ---------------------------------------------------------------------------
include(CTest)

if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.ps1")
    find_program(POWERSHELL_EXECUTABLE NAMES powershell pwsh)
    if(POWERSHELL_EXECUTABLE)
        add_test(NAME end_to_end
                 COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
                         -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.ps1"
                         -BuildDir "${CMAKE_CURRENT_BINARY_DIR}")
        # The suite transfers several hundred megabytes and waits on real
        # network round trips, so it needs a generous timeout.
        set_tests_properties(end_to_end PROPERTIES TIMEOUT 1800)
    else()
        message(STATUS "PowerShell not found: the end-to-end test is not registered")
    endif()
endif()
