cmake_minimum_required(VERSION 3.20)
project(memjet-rip LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(JSL_SDK_DIR "${CMAKE_SOURCE_DIR}/../jsl-sdk")
message(STATUS "JSL SDK: ${JSL_SDK_DIR}")

add_executable(memjet-rip
  memjet-rip.cpp
  pdf_rasterizer.cpp
  bilevel_converter.cpp
  utils.cpp
  jsl_wrapper.cpp
  pes_orchestrator.cpp
  config.cpp
  logger.cpp
  error_codes.cpp
)

target_include_directories(memjet-rip PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${JSL_SDK_DIR}
)

if(WIN32)
  target_compile_definitions(memjet-rip PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)

  find_library(JSL_IMPORT_LIB
    NAMES JobSubmissionLib libJobSubmissionLib
    PATHS ${JSL_SDK_DIR}
    NO_DEFAULT_PATH
  )

  if(NOT JSL_IMPORT_LIB)
    message(FATAL_ERROR "Could not find JobSubmissionLib import library in ${JSL_SDK_DIR}")
  endif()

  target_link_libraries(memjet-rip PRIVATE ws2_32 ${JSL_IMPORT_LIB})
endif()

# Keep runtime DLLs co-located with the executable when available.
set(JSL_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/../vendor/runtime/jsl")
if(EXISTS "${JSL_RUNTIME_DIR}")
  file(GLOB JSL_RUNTIME_DLLS "${JSL_RUNTIME_DIR}/*.dll")
  if(JSL_RUNTIME_DLLS)
    add_custom_command(TARGET memjet-rip POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${JSL_RUNTIME_DLLS}
              $<TARGET_FILE_DIR:memjet-rip>
      COMMENT "Copying JSL runtime DLLs to output directory"
    )
  endif()
endif()

enable_testing()

add_executable(memjet-rip-test-config
  tests_config.cpp
  config.cpp
  utils.cpp
)
target_include_directories(memjet-rip-test-config PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME memjet-rip-test-config COMMAND memjet-rip-test-config)

add_executable(memjet-rip-test-error-codes
  tests_error_codes.cpp
  error_codes.cpp
)
target_include_directories(memjet-rip-test-error-codes PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME memjet-rip-test-error-codes COMMAND memjet-rip-test-error-codes)
