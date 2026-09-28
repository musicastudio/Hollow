# The Hollow core (skin, renderer, State, Editor, custom kinds, platform window) for framework/CMakeLists.txt.
# Needs C++17 (std::filesystem; on macOS a deployment target of 10.15 or later).
#   HOLLOW_CORE_SOURCES   core plus this platform's window code
#   HOLLOW_CORE_INCLUDES  include dirs (public header, src/ for embedded_skin.h, third_party/ for stb)
#   HOLLOW_CORE_LIBS      system libraries to link
#   HOLLOW_RENDER_SOURCES everything the hollow-render tool needs (it defines an empty kSkinFiles)
get_filename_component(HOLLOW_FRAMEWORK_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_core "${HOLLOW_FRAMEWORK_DIR}/src/core")

set(HOLLOW_CORE_SOURCES
    "${_core}/skin.cpp"
    "${_core}/draw.cpp"
    "${_core}/state.cpp"
    "${_core}/editor.cpp"
    "${_core}/kinds.cpp")
set(HOLLOW_CORE_INCLUDES
    "${HOLLOW_FRAMEWORK_DIR}/include"
    "${HOLLOW_FRAMEWORK_DIR}/src"
    "${HOLLOW_FRAMEWORK_DIR}/third_party")

if(WIN32)
    list(APPEND HOLLOW_CORE_SOURCES "${HOLLOW_FRAMEWORK_DIR}/src/platform/win32.cpp")
    set(HOLLOW_CORE_LIBS user32 gdi32 shell32 comctl32 comdlg32)
elseif(APPLE)
    list(APPEND HOLLOW_CORE_SOURCES "${HOLLOW_FRAMEWORK_DIR}/src/platform/mac.mm")
    set(HOLLOW_CORE_LIBS "-framework Cocoa")
else()
    # X11, the window system every Linux plug-in host embeds (a Wayland host runs its plug-ins under XWayland).
    find_package(X11 REQUIRED)
    find_package(Threads REQUIRED)
    list(APPEND HOLLOW_CORE_SOURCES "${HOLLOW_FRAMEWORK_DIR}/src/platform/linux.cpp")
    list(APPEND HOLLOW_CORE_INCLUDES ${X11_INCLUDE_DIR})
    set(HOLLOW_CORE_LIBS ${X11_LIBRARIES} Threads::Threads)
endif()

set(HOLLOW_RENDER_SOURCES "${HOLLOW_FRAMEWORK_DIR}/tools/render.cpp" ${HOLLOW_CORE_SOURCES})
unset(_core)
