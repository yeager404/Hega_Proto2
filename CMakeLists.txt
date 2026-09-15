cmake_minimum_required(VERSION 3.16)
project(Prototype2 CXX)
set(CMAKE_CXX_STANDARD 17)
set(CXX_STANDARD_REQUIRED ON)

# ── OpenFHE ────────────────────────────────────────────────────────────────
find_package(OpenFHE CONFIG REQUIRED)
if(OpenFHE_FOUND)
    message(STATUS "OpenFHE ${BASE_OPENFHE_VERSION} found")
else()
    message(FATAL_ERROR "OpenFHE not found")
endif()

set(CMAKE_CXX_FLAGS "${OpenFHE_CXX_FLAGS}")

include_directories(${OpenFHE_INCLUDE})
include_directories(${OpenFHE_INCLUDE}/third-party/include)
include_directories(${OpenFHE_INCLUDE}/core)
include_directories(${OpenFHE_INCLUDE}/pke)
include_directories(${OpenFHE_INCLUDE}/binfhe)
include_directories(${OPENMP_INCLUDES})

if(UNIX AND NOT APPLE)
    add_link_options(-Wl,--no-as-needed)
endif()

link_directories(${OpenFHE_LIBDIR})
link_directories(${OPENMP_LIBRARIES})
set(CMAKE_EXE_LINKER_FLAGS "${OpenFHE_EXE_LINKER_FLAGS}")

# ── Prototype 1 (reused) ───────────────────────────────────────────────────
set(PROTO1_DIR "${CMAKE_SOURCE_DIR}/../Prototype 1")

include_directories(${PROTO1_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/include)

add_library(proto1 STATIC
    ${PROTO1_DIR}/src/crypto/bfv_context.cpp
    ${PROTO1_DIR}/src/storage/filesystem_storage.cpp
    ${PROTO1_DIR}/src/execution/encrypted_batch.cpp
    ${PROTO1_DIR}/src/execution/operators.cpp
    ${PROTO1_DIR}/src/expression/expression_executor.cpp
    ${PROTO1_DIR}/src/result/result_stream.cpp
    ${PROTO1_DIR}/src/client/client.cpp
    ${PROTO1_DIR}/src/catalog/ingestion.cpp
    ${PROTO1_DIR}/src/planner/plan_validator.cpp
)
target_link_libraries(proto1 PUBLIC ${OpenFHE_SHARED_LIBRARIES})

# ── Library: proto2 ────────────────────────────────────────────────────────
add_library(proto2 STATIC
    ast/ast.cpp
    parser/parser.cpp
    catalog/catalog.cpp
    semantic/semantic.cpp
    planner/planner.cpp
    crypto/crypto_context.cpp
    query/query_package.cpp
    query/query_encryptor.cpp
    query/query_validator.cpp
    execution/query_executor.cpp
    result/result.cpp
    result/result_decoder.cpp
    transport/transport.cpp
    transport/metrics.cpp
)
target_link_libraries(proto2 PUBLIC proto1)

# ── Server executable ──────────────────────────────────────────────────────
add_executable(prototype2_server server/server_main.cpp)
target_link_libraries(prototype2_server PRIVATE proto2)

# ── Client executable ──────────────────────────────────────────────────────
add_executable(prototype2_client client/client_main.cpp)
target_link_libraries(prototype2_client PRIVATE proto2)

# ── Benchmark ─────────────────────────────────────────────────────────────
add_executable(prototype2_benchmark benchmarks/benchmark.cpp)
target_link_libraries(prototype2_benchmark PRIVATE proto2)

# ── GoogleTest ─────────────────────────────────────────────────────────────
set(GTEST_DIR
    ${CMAKE_SOURCE_DIR}/../openfhe-development/third-party/google-test)

if(EXISTS ${GTEST_DIR}/CMakeLists.txt)
    add_subdirectory(${GTEST_DIR} ${CMAKE_BINARY_DIR}/gtest EXCLUDE_FROM_ALL)
    set(GTEST_AVAILABLE TRUE)
    message(STATUS "GoogleTest found at ${GTEST_DIR}")
else()
    find_package(GTest QUIET)
    if(GTest_FOUND)
        set(GTEST_AVAILABLE TRUE)
        message(STATUS "GoogleTest found via find_package")
    else()
        message(WARNING "GoogleTest not found — tests will not be built")
        set(GTEST_AVAILABLE FALSE)
    endif()
endif()

if(GTEST_AVAILABLE)
    enable_testing()

    add_executable(prototype2_tests tests/test_proto2.cpp)

    if(TARGET gtest_main)
        target_link_libraries(prototype2_tests PRIVATE proto2 gtest_main)
    else()
        target_link_libraries(prototype2_tests PRIVATE proto2 GTest::gtest_main)
    endif()

    include(GoogleTest)
    gtest_discover_tests(prototype2_tests)
endif()

# ── Output directories ─────────────────────────────────────────────────────
foreach(tgt prototype2_server prototype2_client prototype2_benchmark)
    set_target_properties(${tgt}
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()

if(GTEST_AVAILABLE)
    set_target_properties(prototype2_tests
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
