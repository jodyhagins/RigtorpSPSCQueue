cmake_minimum_required(VERSION 3.6)

project(RigtorpSPSCQueue VERSION 1.1 LANGUAGES CXX)

if (CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    option(RIGTORP_SPSC_QUEUE_DETERMINE_CACHE_LINE_SIZE
        "Compute value for RIGTORP_SPSC_QUEUE_CACHE_LINE_SIZE if not set"
        ON)
else ()
    option(RIGTORP_SPSC_QUEUE_DETERMINE_CACHE_LINE_SIZE
        "Compute value for RIGTORP_SPSC_QUEUE_CACHE_LINE_SIZE if not set"
        OFF)
endif ()

if (NOT RIGTORP_SPSC_QUEUE_CACHE_LINE_SIZE AND RIGTORP_SPSC_QUEUE_DETERMINE_CACHE_LINE_SIZE)
    if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        execute_process(
            COMMAND sysctl -n hw.cachelinesize
            OUTPUT_VARIABLE RIGTORP_SPSC_QUEUE_CACHE_LINE_SIZE
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    elseif (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
        execute_process(
            COMMAND getconf LEVEL1_DCACHE_LINESIZE
            OUTPUT_VARIABLE RIGTORP_SPSC_QUEUE_CACHE_LINE_SIZE
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    else ()
        message(FATAL_ERROR "Unrecognized CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}")
    endif ()
    message(STATUS
        "Setting RIGTORP_SPSC_QUEUE_CACHE_LINE_SIZE to computed value of "
        ${RIGTORP_SPSC_QUEUE_CACHE_LINE_SIZE}
    )
endif ()

add_library(${PROJECT_NAME} INTERFACE)
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
add_library(Rigtorp::SPSCQueue ALIAS ${PROJECT_NAME})

target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_11)

target_include_directories(${PROJECT_NAME} INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

if (RIGTORP_SPSC_QUEUE_CACHE_LINE_SIZE)
    target_compile_definitions(${PROJECT_NAME} INTERFACE
        RIGTORP_SPSC_QUEUE_CACHE_LINE_SIZE=${RIGTORP_SPSC_QUEUE_CACHE_LINE_SIZE}
    )
else ()
    message(WARNING
        "RIGTORP_SPSC_QUEUE_CACHE_LINE_SIZE will be set to a default value."
    )
endif ()

# Tests and examples
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_library(SPSCQueue ALIAS ${PROJECT_NAME})
    if(MSVC)
        add_compile_options(/permissive- /W4)
    else()
        add_compile_options(-Wall -Wextra -Wpedantic -Wno-c++17-extensions)
    endif()

    find_package(Threads REQUIRED)

    if (UNIX AND NOT APPLE)
        add_executable(SPSCQueueBenchmark src/SPSCQueueBenchmark.cpp)
        target_link_libraries(SPSCQueueBenchmark SPSCQueue Threads::Threads)
    endif()

    if (((CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 7.0)
        OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 9.0))
        AND UNIX AND NOT APPLE)
        add_executable(SPSCQueueExampleHugepages src/SPSCQueueExampleHugepages.cpp)
        target_link_libraries(SPSCQueueExampleHugepages SPSCQueue Threads::Threads)
        target_compile_features(SPSCQueueExampleHugepages PRIVATE cxx_std_17)
    endif()

    add_executable(SPSCQueueExample src/SPSCQueueExample.cpp)
    target_link_libraries(SPSCQueueExample SPSCQueue Threads::Threads)

    add_executable(SPSCQueueTest src/SPSCQueueTest.cpp)
    target_link_libraries(SPSCQueueTest SPSCQueue Threads::Threads)

    enable_testing()
    add_test(SPSCQueueTest SPSCQueueTest)
endif()

# Install
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
  COMPATIBILITY SameMajorVersion
)

export(
    TARGETS ${PROJECT_NAME}
    NAMESPACE ${PROJECT_NAME}::
    FILE "${PROJECT_NAME}Config.cmake"
)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    install(
        DIRECTORY "include/"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(
        TARGETS ${PROJECT_NAME}
        EXPORT "${PROJECT_NAME}Config"
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(
        EXPORT "${PROJECT_NAME}Config"
        NAMESPACE ${PROJECT_NAME}::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
    )

    install(
        FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
    )
endif()
