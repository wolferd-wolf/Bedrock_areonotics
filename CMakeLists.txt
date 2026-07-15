cmake_minimum_required(VERSION 3.22)

project(
    BedrockAeronautics
    VERSION 0.0.1
    DESCRIPTION "Version-locked native Bedrock Android aeronautics mod"
    LANGUAGES CXX
)

option(BEDROCK_AERONAUTICS_BUILD_TESTS "Build host-side unit tests" ON)
set(
    BEDROCK_AERONAUTICS_VERSION
    "${PROJECT_VERSION}-dev"
    CACHE STRING
    "Version string embedded in the native module"
)

add_library(bedrock_aeronautics SHARED
    src/module.cpp
)

add_library(BedrockAeronautics::module ALIAS bedrock_aeronautics)

target_compile_features(bedrock_aeronautics PUBLIC cxx_std_20)
target_include_directories(bedrock_aeronautics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_definitions(bedrock_aeronautics
    PRIVATE
        BEDROCK_AERONAUTICS_VERSION="${BEDROCK_AERONAUTICS_VERSION}"
)

set_target_properties(bedrock_aeronautics PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    OUTPUT_NAME bedrock_aeronautics
)

if(MSVC)
    target_compile_options(bedrock_aeronautics PRIVATE /W4 /permissive-)
else()
    target_compile_options(bedrock_aeronautics PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

if(ANDROID)
    find_library(ANDROID_LOG_LIBRARY log REQUIRED)
    target_link_libraries(bedrock_aeronautics PRIVATE ${ANDROID_LOG_LIBRARY})
endif()

include(CTest)
if(BUILD_TESTING AND BEDROCK_AERONAUTICS_BUILD_TESTS AND NOT ANDROID)
    add_executable(module_contract_test tests/module_contract_test.cpp)
    target_compile_features(module_contract_test PRIVATE cxx_std_20)
    target_link_libraries(module_contract_test PRIVATE BedrockAeronautics::module)
    add_test(NAME module_contract COMMAND module_contract_test)
endif()
