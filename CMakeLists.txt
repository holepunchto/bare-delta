cmake_minimum_required(VERSION 3.25)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)

project(bare_delta C ASM)

fetch_package("github:holepunchto/libcompact")
fetch_package("github:holepunchto/libsimdle")
fetch_package("github:facebook/zstd#v1.5.7" SOURCE_DIR zstd_source)

file(GLOB zstd_common_sources ${zstd_source}/lib/common/*.c)
file(GLOB zstd_compress_sources ${zstd_source}/lib/compress/*.c)  
file(GLOB zstd_decompress_sources ${zstd_source}/lib/decompress/*.c)
file(GLOB zstd_assembly_sources ${zstd_source}/lib/decompress/*.S)

add_library(zstd STATIC)

target_sources(
  zstd
  PRIVATE
    ${zstd_common_sources}
    ${zstd_compress_sources}
    ${zstd_decompress_sources}
    ${zstd_assembly_sources}
)

add_library(delta STATIC)

target_sources(
  delta
  PRIVATE
    delta.c
)

target_link_libraries(
  delta
  PUBLIC
    compact
    simdle
)

add_bare_module(bare_delta)

target_sources(
  ${bare_delta}
  PRIVATE
    binding.c
)

target_include_directories(
  ${bare_delta}
  PRIVATE
    ${zstd_source}/lib
)

target_link_libraries(
  ${bare_delta}
  PRIVATE
    $<TARGET_OBJECTS:simdle>
    $<TARGET_OBJECTS:compact>
  PUBLIC
    simdle
    compact
    zstd
    delta
)
