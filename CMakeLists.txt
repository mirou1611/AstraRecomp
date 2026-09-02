cmake_minimum_required(VERSION 3.18)

if(DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
endif()

project(ps2vita LANGUAGES C CXX)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ASTRART_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ASTRART_SMOKE_ELF "${ASTRART_GENERATED_DIR}/astrarecomp-smoke.elf")
set(ASTRART_CHAIN_ELF "${ASTRART_GENERATED_DIR}/aot-chain.elf")
set(ASTRART_MEMORY_ELF "${ASTRART_GENERATED_DIR}/aot-memory.elf")
set(ASTRART_UNSUPPORTED_ELF "${ASTRART_GENERATED_DIR}/aot-unsupported.elf")
set(ASTRART_BRANCH_ELF "${ASTRART_GENERATED_DIR}/aot-branch.elf")
set(ASTRART_BNE_ELF "${ASTRART_GENERATED_DIR}/aot-bne.elf")
set(ASTRART_HOT_SCALAR_ELF "${ASTRART_GENERATED_DIR}/aot-hot-scalars.elf")
set(ASTRART_BENCHMARK_ELF "${ASTRART_GENERATED_DIR}/aot-benchmark.elf")
set(ASTRART_BENCHMARK_HEADER "${ASTRART_GENERATED_DIR}/aot_benchmark_program.hpp")
set(ASTRART_PHASE0_CPP "${ASTRART_GENERATED_DIR}/phase0_aot_package.cpp")
add_custom_command(
  OUTPUT "${ASTRART_PHASE0_CPP}" "${ASTRART_BENCHMARK_HEADER}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${ASTRART_GENERATED_DIR}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_smoke_elf.py" "${ASTRART_SMOKE_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_chain_elf.py" "${ASTRART_CHAIN_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_memory_elf.py" "${ASTRART_MEMORY_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_memory_elf.py" --unsupported "${ASTRART_UNSUPPORTED_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_memory_elf.py" --branches "${ASTRART_BRANCH_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_memory_elf.py" --branches-bne "${ASTRART_BNE_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_memory_elf.py" --hot-scalars "${ASTRART_HOT_SCALAR_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_benchmark_elf.py"
          "${ASTRART_BENCHMARK_ELF}" --header "${ASTRART_BENCHMARK_HEADER}"
  COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
          "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_astrart.py"
          --output "${ASTRART_PHASE0_CPP}"
          "${ASTRART_SMOKE_ELF}" "${ASTRART_CHAIN_ELF}"
          "${ASTRART_MEMORY_ELF}" "${ASTRART_UNSUPPORTED_ELF}"
          "${ASTRART_BRANCH_ELF}" "${ASTRART_BNE_ELF}"
          "${ASTRART_HOT_SCALAR_ELF}" "${ASTRART_BENCHMARK_ELF}"
  DEPENDS
    tools/generate_astrart.py
    tools/astrair/__init__.py
    tools/astrair/ir.py
    tools/astrair/builder.py
    tools/astrair/analysis_effects.py
    tools/astrair/analysis_width.py
    tools/astrair/analysis_liveness.py
    tools/make_smoke_elf.py
    tools/make_aot_chain_elf.py
    tools/make_aot_memory_elf.py
    tools/make_aot_benchmark_elf.py
  VERBATIM
)

add_library(ps2core
  "${ASTRART_PHASE0_CPP}"
  "${ASTRART_BENCHMARK_HEADER}"
  src/core/aot.cpp
  src/core/cpu.cpp
  src/core/ee_block.cpp
  src/core/elf_loader.cpp
  src/core/emulator.cpp
  src/core/gs.cpp
  src/core/iop_cpu.cpp
  src/core/memory.cpp
)
if(NOT VITA)
  target_sources(ps2core PRIVATE src/core/execution_census.cpp)
endif()
target_include_directories(ps2core PUBLIC include PRIVATE "${ASTRART_GENERATED_DIR}")
if(MSVC)
  target_compile_options(ps2core PRIVATE /W4)
else()
  target_compile_options(ps2core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(VITA)
  include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

  set(VITA_APP_NAME "AstraRecomp")
  set(VITA_TITLEID "PS2VITA01")
  set(VITA_VERSION "01.00")

  add_executable(ps2vita
    src/vita/main.cpp
    src/vita/screen.cpp
  )
  target_include_directories(ps2vita PRIVATE include src/vita)
  target_compile_options(ps2vita PRIVATE -Wall -Wextra -Wpedantic -O2)
  target_link_libraries(ps2vita PRIVATE
    ps2core
    SceCtrl_stub
    SceDisplay_stub
    SceIofilemgr_stub
    SceKernelThreadMgr_stub
    SceProcessmgr_stub
    SceSysmem_stub
  )

  vita_create_self(ps2vita.self ps2vita UNSAFE)
  vita_create_vpk(ps2vita.vpk ${VITA_TITLEID} ps2vita.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
  )
else()
  enable_testing()
  add_executable(ps2core_tests tests/core_tests.cpp)
  target_link_libraries(ps2core_tests PRIVATE ps2core)
  add_executable(ps2bios_trace tools/bios_trace.cpp)
  target_link_libraries(ps2bios_trace PRIVATE ps2core)
  if(MSVC)
    target_compile_options(ps2core_tests PRIVATE /W4)
    target_compile_options(ps2bios_trace PRIVATE /W4)
  else()
    target_compile_options(ps2core_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ps2bios_trace PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ps2core_tests COMMAND ps2core_tests)
  add_test(NAME astrair_builder_tests
           COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
                   "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_astrair.py")
endif()
