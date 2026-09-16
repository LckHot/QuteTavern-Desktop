# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(qutetavern VERSION 1.0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
    # Without this MSVC assumes the local code page and would garble the Unicode
    # symbols in the UI strings (and emit C4819 warnings at best)
    add_compile_options(/utf-8)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets Network)

qt_add_executable(qutetavern
    src/main.cpp
    src/MainWindow.cpp
    src/Backend.cpp
    src/StWindow.cpp
    src/Settings.cpp
    src/Util.cpp
    src/Updater.cpp
    src/Installer.cpp
    src/EnvCheck.cpp
    resources/icons.qrc
)

target_link_libraries(qutetavern PRIVATE
    Qt6::Widgets
    Qt6::WebEngineWidgets
    Qt6::Network
)

# Keep the tree warning-free; the CI compilers are GCC/Clang (Linux, macOS) and
# MSVC (Windows). See CODE-REVIEW.md (F8/F9) for the findings this surfaced.
target_compile_options(qutetavern PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)

install(TARGETS qutetavern RUNTIME DESTINATION bin)

# Bundle metadata used by the packaging jobs in .github/workflows/build.yml
if(APPLE)
    set_target_properties(qutetavern PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "QuteTavern"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.lckhot.qutetavern"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
elseif(WIN32)
    set_target_properties(qutetavern PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# ---------- unit tests (ctest) ----------
# Pure-logic tests, no GUI: the parts that break silently. They reuse the same
# source files as the app so both stay honest about the same code.
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(qutetavern-tests
        tests/tst_qutetavern.cpp
        src/Util.cpp
        src/Updater.cpp
    )
    target_include_directories(qutetavern-tests PRIVATE src)
    target_compile_options(qutetavern-tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
    )
    target_link_libraries(qutetavern-tests PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Test
    )
    add_test(NAME qutetavern-tests COMMAND qutetavern-tests)
endif()