cmake_minimum_required(VERSION 3.25)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)

project(bare_delta C)

# Create the bare module
add_bare_module(bare_delta)

# Add our binding source and delta source
target_sources(
  ${bare_delta}
  PRIVATE
    binding.c
    delta.c
)