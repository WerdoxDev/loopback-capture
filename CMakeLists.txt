cmake_minimum_required(VERSION 3.15...3.31)
project(loopback_capture_addon LANGUAGES CXX)

add_compile_definitions(NAPI_VERSION=9)

set(COMMON_SOURCES
    src/Addon.cpp
)

if(WIN32)
    include(FetchContent)
    FetchContent_Declare(
        wil
        GIT_REPOSITORY https://github.com/microsoft/wil.git
        GIT_TAG master
        EXCLUDE_FROM_ALL
        SYSTEM
    )
    set(WIL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WIL_BUILD_PACKAGING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(wil)

    set(PLATFORM_DIR src/platform/windows)
    set(PLATFORM_SOURCES
        ${PLATFORM_DIR}/LoopbackCapture.cpp
        ${PLATFORM_DIR}/LoopbackCaptureWrap.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

    set(PLATFORM_DIR src/platform/linux)
    set(PLATFORM_SOURCES
        ${PLATFORM_DIR}/LoopbackCapture.cpp
        ${PLATFORM_DIR}/LoopbackCaptureWrap.cpp
    )
else()
    message(FATAL_ERROR "loopback-capture supports Windows and Linux only")
endif()

add_library(${PROJECT_NAME} SHARED
    ${COMMON_SOURCES}
    ${PLATFORM_SOURCES}
    ${CMAKE_JS_SRC}
)
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" SUFFIX ".node")

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_JS_INC}
    ${PLATFORM_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_JS_LIB})

if(WIN32)
    target_include_directories(${PROJECT_NAME} PRIVATE ${wil_SOURCE_DIR}/include)
    target_link_libraries(${PROJECT_NAME} PRIVATE windowsapp.lib mfuuid.lib)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::PIPEWIRE)
endif()

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

if(MSVC AND CMAKE_JS_NODELIB_DEF AND CMAKE_JS_NODELIB_TARGET)
    # Generate node.lib.
    execute_process(
        COMMAND ${CMAKE_AR} /def:${CMAKE_JS_NODELIB_DEF} /out:${CMAKE_JS_NODELIB_TARGET} ${CMAKE_STATIC_LINKER_FLAGS}
    )
endif()
