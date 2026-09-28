# Build settings every Hollow product needs. Include it before project():
#   include(<hollow>/framework/cmake/HollowDefaults.cmake)
if(APPLE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
# Plug-ins carry their own C++ runtime, and clap-wrapper's libraries are built the same way.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(CMAKE_GENERATOR MATCHES "Visual Studio")
  set(CMAKE_C_FLAGS_INIT "/utf-8 /MP")
  set(CMAKE_CXX_FLAGS_INIT "/utf-8 /MP")
endif()
