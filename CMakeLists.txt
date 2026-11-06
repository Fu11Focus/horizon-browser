cmake_minimum_required(VERSION 3.21)
project(minibrowser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# WKWebView `inspectable` requires macOS 13.3+. Keep a modern deployment target.
if (APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS deployment version")
    enable_language(OBJCXX) # for src/titlebar_mac.mm
endif ()

include(cmake/CPM.cmake)

CPMFindPackage(
    NAME           saucer
    VERSION        8.0.5
    GIT_REPOSITORY "https://github.com/saucer/saucer"
)

add_executable(minibrowser MACOSX_BUNDLE
    src/main.cpp
)

# App icon (artwork + generator: assets/make_icons.py).
if (APPLE)
    set(MB_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.icns")
    set_source_files_properties(${MB_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(minibrowser PRIVATE ${MB_ICNS})
    set_target_properties(minibrowser PROPERTIES MACOSX_BUNDLE_ICON_FILE icon.icns)
elseif (WIN32)
    target_sources(minibrowser PRIVATE assets/icon.rc)
endif ()

if (APPLE)
    target_sources(minibrowser PRIVATE src/platform_mac.mm)
    target_link_libraries(minibrowser PRIVATE "-framework Cocoa" "-framework WebKit" "-framework Security")
endif ()

# Embed the chrome UI (content/) directly into the binary — single contained
# executable, served at runtime via saucer's embedded scheme.
saucer_embed("content" TARGET minibrowser)

target_link_libraries(minibrowser PRIVATE saucer::saucer saucer::embedded)
