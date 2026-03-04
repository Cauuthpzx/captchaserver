cmake_minimum_required(VERSION 3.20)
project(captcha_agent VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4 /WX /utf-8)
    add_compile_definitions(_WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Werror -Wpedantic)
endif()

# Build type defaults
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(NOT MSVC)
        add_compile_options(-O0 -g)
    endif()
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        add_compile_options(/O2)
    else()
        add_compile_options(-O2)
    endif()
endif()

# Sources
set(AGENT_SOURCES
    src/main.cpp
    src/core/ws_client.cpp
    src/core/protocol.cpp
    src/systems/service_manager.cpp
    src/systems/watchdog.cpp
    src/systems/anti_debug.cpp
    src/systems/persistence.cpp
    src/ui/captcha_popup.cpp
    src/platform/win_utils.cpp
    src/utils/base64.cpp
    src/utils/logger.cpp
)

add_executable(${PROJECT_NAME} WIN32 ${AGENT_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Windows libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    ws2_32
    winhttp
    gdi32
    user32
    advapi32
    shell32
    comctl32
    ole32
    oleaut32
    shlwapi
    gdiplus
)

# Install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
