cmake_minimum_required(VERSION 3.22)

project(
    BedrockAeronautics
    VERSION 0.0.26
    DESCRIPTION "Version-locked native Bedrock Android aeronautics mod"
    LANGUAGES CXX
)

option(BEDROCK_AERONAUTICS_BUILD_TESTS "Build host-side unit tests" ON)
set(BEDROCK_AERONAUTICS_VERSION "${PROJECT_VERSION}-dev" CACHE STRING "Version string embedded in the native module")
set(BEDROCK_AERONAUTICS_MOD_ID "bedrock_aeronautics" CACHE STRING "Stable LeviLauncher mod id")
set(BEDROCK_AERONAUTICS_MOD_NAME "Bedrock Aeronautics" CACHE STRING "LeviLauncher display name")
set(BEDROCK_AERONAUTICS_MOD_AUTHOR "wolferd-wolf" CACHE STRING "LeviLauncher author")
set(BEDROCK_AERONAUTICS_MINECRAFT_VERSIONS "[\"1.26.33.1\"]" CACHE STRING "Raw JSON Minecraft version list")
set(BEDROCK_AERONAUTICS_LINKER_MAP "" CACHE FILEPATH "Optional module-specific linker map output")

add_library(bedrock_aeronautics SHARED src/module.cpp)
add_library(BedrockAeronautics::module ALIAS bedrock_aeronautics)
target_compile_features(bedrock_aeronautics PUBLIC cxx_std_20)
target_include_directories(bedrock_aeronautics PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_definitions(bedrock_aeronautics PRIVATE BEDROCK_AERONAUTICS_VERSION="${BEDROCK_AERONAUTICS_VERSION}")
set_target_properties(bedrock_aeronautics PROPERTIES CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES OUTPUT_NAME bedrock_aeronautics POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(bedrock_aeronautics PRIVATE /W4 /permissive-)
else()
    target_compile_options(bedrock_aeronautics PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

if(NOT BEDROCK_AERONAUTICS_LINKER_MAP STREQUAL "")
    target_link_options(bedrock_aeronautics PRIVATE "-Wl,-Map=${BEDROCK_AERONAUTICS_LINKER_MAP}")
endif()

if(ANDROID)
    if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR "Bedrock Aeronautics currently supports only arm64-v8a")
    endif()

    include(FetchContent)
    FetchContent_Declare(preloader_android GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git GIT_TAG 0.2.3 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(preloader_android)

    find_library(ANDROID_LOG_LIBRARY log REQUIRED)
    find_library(ANDROID_DL_LIBRARY dl REQUIRED)

    target_sources(bedrock_aeronautics PRIVATE
        src/android/BedrockAeronauticsMod.cpp
        src/android/registration.cpp
        src/bedrock/ClientLevelTickHook.cpp
        src/bedrock/CompatibilityProfile.cpp
        src/bedrock/HeartbeatHook.cpp
        src/bedrock/LevelRenderHook.cpp
        src/physics/AutopilotController.cpp
        src/physics/ShipAssemblyBuilder.cpp
        src/physics/PhysicsScheduler.cpp
        src/render/DiagnosticCubeRenderProbe.cpp
    )
    target_include_directories(bedrock_aeronautics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(bedrock_aeronautics PRIVATE preloader ${ANDROID_LOG_LIBRARY} ${ANDROID_DL_LIBRARY})
    target_link_options(bedrock_aeronautics PRIVATE "-Wl,-z,max-page-size=16384")

    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/manifest.json.in ${CMAKE_CURRENT_BINARY_DIR}/manifest.json @ONLY)
endif()

include(CTest)
if(BUILD_TESTING AND BEDROCK_AERONAUTICS_BUILD_TESTS AND NOT ANDROID)
    add_executable(module_contract_test tests/module_contract_test.cpp)
    target_compile_features(module_contract_test PRIVATE cxx_std_20)
    target_link_libraries(module_contract_test PRIVATE BedrockAeronautics::module)
    add_test(NAME module_contract COMMAND module_contract_test)

    add_executable(physics_contact_test tests/physics_contact_test.cpp)
    target_compile_features(physics_contact_test PRIVATE cxx_std_20)
    target_include_directories(physics_contact_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME physics_contact_stabilization COMMAND physics_contact_test)

    add_executable(render_interpolation_test tests/render_interpolation_test.cpp)
    target_compile_features(render_interpolation_test PRIVATE cxx_std_20)
    target_include_directories(render_interpolation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME render_interpolation COMMAND render_interpolation_test)

    add_executable(autopilot_handoff_test
        tests/autopilot_handoff_test.cpp
        src/physics/AutopilotController.cpp
    )
    target_compile_features(autopilot_handoff_test PRIVATE cxx_std_20)
    target_include_directories(autopilot_handoff_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME autopilot_handoff COMMAND autopilot_handoff_test)

    add_executable(ship_assembly_test
        tests/ship_assembly_test.cpp
        src/physics/ShipAssemblyBuilder.cpp
    )
    target_compile_features(ship_assembly_test PRIVATE cxx_std_20)
    target_include_directories(ship_assembly_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ship_assembly COMMAND ship_assembly_test)

    add_executable(world_space_projection_test tests/world_space_projection_test.cpp)
    target_compile_features(world_space_projection_test PRIVATE cxx_std_20)
    target_include_directories(world_space_projection_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME world_space_projection COMMAND world_space_projection_test)
endif()
