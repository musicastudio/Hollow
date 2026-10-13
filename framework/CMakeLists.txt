# The framework: hollow_core and hollow-render (their sources are listed by src/core/sources.cmake),
# the CLAP SDK and clap-wrapper, and hollow_add_plugin() for the products.
include(cmake/HollowEmbed.cmake)
include(cmake/HollowPlugin.cmake)

set(HOLLOW_CORE_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/src/core/sources.cmake" CACHE FILEPATH
  "Lists HOLLOW_CORE_SOURCES, HOLLOW_CORE_INCLUDES, HOLLOW_CORE_LIBS and HOLLOW_RENDER_SOURCES")
include("${HOLLOW_CORE_CMAKE}")

add_library(hollow_core STATIC ${HOLLOW_CORE_SOURCES})
target_include_directories(hollow_core PUBLIC include src ${HOLLOW_CORE_INCLUDES})
target_link_libraries(hollow_core PUBLIC ${HOLLOW_CORE_LIBS})

# hollow-render works on skin folders; its sources include the core and an empty skin table.
add_executable(hollow-render ${HOLLOW_RENDER_SOURCES})
target_include_directories(hollow-render PRIVATE include src ${HOLLOW_CORE_INCLUDES})
target_link_libraries(hollow-render PRIVATE ${HOLLOW_CORE_LIBS})

# hollow-bake burns a skin's surface into its artwork (a skin published without its runtime surface).
add_executable(hollow-bake "${CMAKE_CURRENT_SOURCE_DIR}/tools/bake.cpp" ${HOLLOW_CORE_SOURCES})
target_include_directories(hollow-bake PRIVATE include src ${HOLLOW_CORE_INCLUDES})
target_link_libraries(hollow-bake PRIVATE ${HOLLOW_CORE_LIBS})

# The CLAP SDK and clap-wrapper, which fetches the VST3 SDK, AudioUnitSDK, RtAudio and RtMidi
# itself. The 32-bit Windows build is VST2 and DXi only.
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND HOLLOW_FORMATS MATCHES "CLAP")
  include(FetchContent)
  set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "clap-wrapper downloads the SDKs it needs")
  # RtAudio builds WASAPI only unless asked for more, and it vendors the ASIO SDK. FORCE because it
  # declares the flag with option() under cmake_minimum_required(3.10), where CMP0077 is OLD.
  if(WIN32)
    option(HOLLOW_STANDALONE_ASIO "Build the standalone's ASIO back end" ON)
    if(HOLLOW_STANDALONE_ASIO)
      set(RTAUDIO_API_ASIO ON CACHE BOOL "Build ASIO API" FORCE)
    endif()
  endif()
  FetchContent_Declare(clap GIT_REPOSITORY https://github.com/free-audio/clap GIT_TAG 1.2.10 GIT_SHALLOW TRUE)
  # Ahead of v0.16.0: the standalone's audio and MIDI settings only persist from PR #517, which is
  # merged onto next but not released. Move back to a tag (v0.17.0?) once one carries it. The sha is
  # spelled in full because GitHub serves a fetch of nothing shorter.
  #
  # Not shallow, unlike clap above, which is pinned to a tag. GIT_SHALLOW clones at depth 1, which holds
  # each branch's tip and nothing behind it, so a sha only checks out while it is still the tip of next:
  # one commit upstream and the clone no longer contains it ("unable to read tree"), which is a build that
  # breaks on its own with nothing changed here. A full clone can check out any commit that is still
  # reachable, so the pin keeps meaning what it says.
  FetchContent_Declare(clap-wrapper GIT_REPOSITORY https://github.com/free-audio/clap-wrapper GIT_TAG 527a37461dd0480562d110e857cf9538fecd8b98)
  FetchContent_MakeAvailable(clap clap-wrapper)
  include(cmake/HollowClapWrapperFix.cmake)
  hollow_fix_clap_wrapper("${clap-wrapper_SOURCE_DIR}")
endif()
