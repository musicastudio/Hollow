# Hollow: the framework and FSVR's GUI shell. Windows x64 builds CLAP, VST3, VST2 and a standalone;
# -A Win32 builds the 32-bit VST2 with the DXi in the same DLL; macOS builds universal CLAP, VST3,
# VST2, AU and a standalone app.
cmake_minimum_required(VERSION 3.22)
include(framework/cmake/HollowDefaults.cmake)

project(Hollow VERSION 1.0.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

add_subdirectory(framework)
add_subdirectory(plugins/fsvr)
