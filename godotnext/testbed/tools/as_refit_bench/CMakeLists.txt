# The AS calibration refit tool (#9, G0-6). Standalone: not part of the engine build.
# Build it outside the repository (see README.md); it needs the Vulkan SDK.
cmake_minimum_required(VERSION 3.20)
project(as_refit_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Vulkan 1.3 REQUIRED)

add_executable(as_refit_bench as_refit_bench.cpp)
target_link_libraries(as_refit_bench PRIVATE Vulkan::Vulkan)
if(MSVC)
	target_compile_options(as_refit_bench PRIVATE /W4 /permissive- /utf-8)
	target_compile_definitions(as_refit_bench PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
	target_compile_options(as_refit_bench PRIVATE -Wall -Wextra)
endif()

# The CPU-only self-test: meshes, percentiles, options and the result's keys. No GPU needed.
enable_testing()
add_test(NAME as_refit_bench_self_test COMMAND as_refit_bench --self-test)
