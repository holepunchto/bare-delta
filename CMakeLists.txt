cmake_minimum_required(VERSION 3.25)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)

project(bare_delta C)

# Fetch libcompact for modern uint encoding
fetch_package("github:holepunchto/libcompact" SOURCE_DIR libcompact_source)
fetch_package("github:holepunchto/libsimdle" SOURCE_DIR libsimdle_source)
fetch_package("github:facebook/zstd#v1.5.7" SOURCE_DIR zstd_source)

# Create the bare module
add_bare_module(bare_delta)

# Collect dependency source files using glob patterns
file(GLOB libcompact_sources ${libcompact_source}/src/*.c)
file(GLOB zstd_common_sources ${zstd_source}/lib/common/*.c)
file(GLOB zstd_compress_sources ${zstd_source}/lib/compress/*.c)  
file(GLOB zstd_decompress_sources ${zstd_source}/lib/decompress/*.c)

# Add our binding source, delta source, and dependency sources
target_sources(
  ${bare_delta}
  PRIVATE
    binding.c
    delta.c
    ${libcompact_sources}
    ${zstd_common_sources}
    ${zstd_compress_sources}
    ${zstd_decompress_sources}
)

# Include directories for libcompact, libsimdle, zstd, and libutf headers
target_include_directories(
  ${bare_delta}
  PRIVATE
    ${libcompact_source}/include
    ${libsimdle_source}/include
    ${zstd_source}/lib
    build/_deps/github+holepunchto+libutf-src/include
)