cmake_minimum_required(VERSION 3.21)
project(horizon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# WKWebView `inspectable` requires macOS 13.3+. Keep a modern deployment target.
if (APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS deployment version")
    enable_language(OBJCXX) # for src/platform_mac.mm
endif ()

include(cmake/CPM.cmake)

CPMFindPackage(
    NAME           saucer
    VERSION        8.0.5
    GIT_REPOSITORY "https://github.com/saucer/saucer"
)

add_executable(Horizon MACOSX_BUNDLE
    src/main.cpp
)

# App icon (artwork + generator: assets/make_icons.py).
if (APPLE)
    set(MB_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.icns")
    set_source_files_properties(${MB_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(Horizon PRIVATE ${MB_ICNS})
    # Bundle identity — without these CMake emits an empty CFBundleIdentifier /
    # CFBundleName, which makes Spotlight and Launch Services unreliable.
    set_target_properties(Horizon PROPERTIES
        MACOSX_BUNDLE_ICON_FILE            icon.icns
        MACOSX_BUNDLE_GUI_IDENTIFIER       "com.dreamvention.horizon"
        MACOSX_BUNDLE_BUNDLE_NAME          "Horizon"
        MACOSX_BUNDLE_BUNDLE_VERSION       "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0")
elseif (WIN32)
    target_sources(Horizon PRIVATE assets/icon.rc)
endif ()

if (APPLE)
    target_sources(Horizon PRIVATE src/platform_mac.mm)
    target_link_libraries(Horizon PRIVATE "-framework Cocoa" "-framework WebKit" "-framework Security")
endif ()

# Embed the chrome UI (content/) directly into the binary — single contained
# executable, served at runtime via saucer's embedded scheme.
saucer_embed("content" TARGET Horizon)

target_link_libraries(Horizon PRIVATE saucer::saucer saucer::embedded)

# Copy the freshly built bundle to /Applications after each build so it stays
# launchable from Spotlight (the app bundle lives in the build tree otherwise).
# Re-sign ad-hoc (the copy + any plist changes need a valid signature) and
# re-register with Launch Services. Best-effort: never fail the build if
# /Applications isn't writable. Disable with -DHORIZON_INSTALL_TO_APPLICATIONS=OFF.
option(HORIZON_INSTALL_TO_APPLICATIONS "Copy Horizon.app to /Applications after build" ON)
if (APPLE AND HORIZON_INSTALL_TO_APPLICATIONS)
    set(_lsregister "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister")
    add_custom_command(TARGET Horizon POST_BUILD
        COMMAND bash -c "rm -rf /Applications/Horizon.app \
            && cp -R '$<TARGET_BUNDLE_DIR:Horizon>' /Applications/ \
            && codesign --force --deep --sign - /Applications/Horizon.app \
            && '${_lsregister}' -f /Applications/Horizon.app \
            && echo 'Installed Horizon.app to /Applications' \
            || echo 'WARNING: could not install Horizon.app to /Applications (continuing)'"
        VERBATIM
        COMMENT "Installing Horizon.app to /Applications")
endif ()
