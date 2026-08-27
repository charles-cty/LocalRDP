cmake_minimum_required(VERSION 3.21)

project(LocalRDP VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "LocalRDP is a Windows-only tool")
endif()

add_executable(localrdp
    src/main.cpp
    src/log.cpp
    src/win_util.cpp
    src/dispatch.cpp
    src/env.cpp
    src/host_prep.cpp
    src/job.cpp
    src/rdp_events.cpp
    src/rdp_host.cpp
    src/start.cpp
    src/session_entry.cpp
    src/session_launch.cpp
    src/clip_bridge.cpp
    src/localrdp.rc
)

target_include_directories(localrdp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(localrdp PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _ATL_NO_AUTOMATIC_NAMESPACE
    _ATL_CSTRING_EXPLICIT_CONSTRUCTORS
    LOCALRDP_VERSION_STRING="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(localrdp PRIVATE
        /W4
        /EHsc
        /utf-8
        /Zc:__cplusplus
        /permissive-
    )
    # Our .rc embeds the application manifest. Stop MSBuild/link from adding a second one.
    set_target_properties(localrdp PROPERTIES
        VS_GLOBAL_GenerateManifest "false"
    )
    target_link_options(localrdp PRIVATE /MANIFEST:NO)
endif()

target_link_libraries(localrdp PRIVATE
    wtsapi32
    ole32
    oleaut32
    uuid
    user32
    gdi32
    advapi32
    shell32
    comctl32
    oleacc
    rpcrt4
    shlwapi
    atls
    userenv
)

set_target_properties(localrdp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin/RelWithDebInfo"
)
