cmake_minimum_required(VERSION 3.20)
project(FluxIO VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(fluxio INTERFACE)
add_library(FluxIO::fluxio ALIAS fluxio)
target_include_directories(fluxio INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(fluxio INTERFACE
    pthread
)

target_compile_options(fluxio INTERFACE
    -Wall
    -Wextra
    -Wno-interference-size
    -O3
)

find_library(URING_LIB uring)
find_path(URING_INCLUDE liburing.h)

if(URING_LIB AND URING_INCLUDE)
    message(STATUS "FluxIO: liburing found at ${URING_LIB}. Enabling io_uring backend.")
    target_compile_definitions(fluxio INTERFACE FLUX_HAS_IO_URING=1)
    target_link_libraries(fluxio INTERFACE ${URING_LIB})
else()
    message(WARNING "FluxIO: liburing NOT found. Falling back to POSIX Direct I/O backend.")
    target_compile_definitions(fluxio INTERFACE FLUX_HAS_IO_URING=0)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmark/bench_fluxio.cpp")
    add_executable(bench_fluxio benchmark/bench_fluxio.cpp)
    target_link_libraries(bench_fluxio PRIVATE fluxio)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/user.cpp")
    add_executable(user_demo user.cpp)
    target_link_libraries(user_demo PRIVATE fluxio)
endif()

# Examples (Full Suite)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/io_uring_example.cpp" AND URING_LIB)
    add_executable(io_uring_example examples/io_uring_example.cpp)
    target_link_libraries(io_uring_example PRIVATE fluxio)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/posix_example.cpp")
    add_executable(posix_example examples/posix_example.cpp)
    target_link_libraries(posix_example PRIVATE fluxio)
endif()

# Separate io_uring Examples
if(URING_LIB)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/io_uring_write.cpp")
        add_executable(io_uring_write examples/io_uring_write.cpp)
        target_link_libraries(io_uring_write PRIVATE fluxio)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/io_uring_read.cpp")
        add_executable(io_uring_read examples/io_uring_read.cpp)
        target_link_libraries(io_uring_read PRIVATE fluxio)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/io_uring_fsync.cpp")
        add_executable(io_uring_fsync examples/io_uring_fsync.cpp)
        target_link_libraries(io_uring_fsync PRIVATE fluxio)
    endif()
endif()

# Separate POSIX Examples
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/posix_write.cpp")
    add_executable(posix_write examples/posix_write.cpp)
    target_link_libraries(posix_write PRIVATE fluxio)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/posix_read.cpp")
    add_executable(posix_read examples/posix_read.cpp)
    target_link_libraries(posix_read PRIVATE fluxio)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/posix_fsync.cpp")
    add_executable(posix_fsync examples/posix_fsync.cpp)
    target_link_libraries(posix_fsync PRIVATE fluxio)
endif()

# Installation & Export
include(GNUInstallDirs)
install(TARGETS fluxio
    EXPORT FluxIOTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT FluxIOTargets
    FILE FluxIOTargets.cmake
    NAMESPACE FluxIO::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FluxIO
)
