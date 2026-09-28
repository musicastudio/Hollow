# hollow_add_plugin(<target>
#     NAME "My Synth"                      file and display name of every format
#     SKIN <dir>                           the skin folder, embedded into the binaries
#     SOURCES plugin.cpp ...               pluginInfo(), createProcessor() and any DSP
#     BUNDLE_ID com.example.my-synth       must equal Info::id; the CLAP id and the bundle id
#     [VERSION 1.0.0]                      bundle version (default: the project version)
#     [VENDOR "musica.studio"]             AU manufacturer name
#     [AU_TYPE aumu|aufx] [AU_MANUFACTURER <4 chars>] [AU_SUBTYPE <4 chars>]
#     [DXI_CLSID <uuid>]                   default: a name-based UUID of BUNDLE_ID
#     [BIN <dir>])                         also copy each format, as built, into <dir>/<format>
#
# Builds every format the platform has, limited to the list in HOLLOW_FORMATS:
#   Windows x64: CLAP, VST3, standalone (clap-wrapper), VST2
#   Windows x86: VST2 with the DXi in the same DLL
#   macOS:       CLAP, VST3, AUv2, standalone .app (clap-wrapper), VST2 .vst bundle
#   Linux:       CLAP, VST3, standalone (clap-wrapper), VST2 .so; the editor is an X11 window
# Everything lands under <build>/out/ (per-format folders on Windows; VST2 in out/VST2 everywhere).
# The DXi needs VST2 in the list, since it lives in that DLL.
set(HOLLOW_FORMATS "CLAP;VST3;AUV2;STANDALONE;VST2;DXI" CACHE STRING "Formats to build, where the platform has them")

function(hollow_add_plugin target)
  cmake_parse_arguments(P "" "NAME;SKIN;BUNDLE_ID;VERSION;VENDOR;AU_TYPE;AU_MANUFACTURER;AU_SUBTYPE;DXI_CLSID;BIN" "SOURCES" ${ARGN})
  foreach(arg NAME SKIN BUNDLE_ID SOURCES)
    if(NOT P_${arg})
      message(FATAL_ERROR "hollow_add_plugin(${target}): ${arg} is required")
    endif()
  endforeach()
  if(NOT P_VERSION)
    set(P_VERSION "${PROJECT_VERSION}")
  endif()
  if(NOT P_VENDOR)
    set(P_VENDOR "Hollow")
  endif()
  if(NOT P_AU_TYPE)
    set(P_AU_TYPE aumu)
  endif()
  if(NOT P_AU_MANUFACTURER)
    set(P_AU_MANUFACTURER Hlow)
  endif()
  if(NOT P_AU_SUBTYPE)
    string(SHA1 hash "${P_BUNDLE_ID}")
    string(SUBSTRING "${hash}" 0 4 P_AU_SUBTYPE)
  endif()
  set(fw "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
  set(out "${CMAKE_BINARY_DIR}/out")

  # The product: its own sources plus its skin, on the core.
  hollow_embed_skin("${P_SKIN}" "${CMAKE_CURRENT_BINARY_DIR}/${target}_skin.cpp")
  add_library(${target}_lib STATIC ${P_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/${target}_skin.cpp")
  target_link_libraries(${target}_lib PUBLIC hollow_core)

  # CLAP, and through clap-wrapper VST3, AUv2 and the standalone (64-bit only, see framework/CMakeLists.txt).
  if(COMMAND make_clapfirst_plugins AND "CLAP" IN_LIST HOLLOW_FORMATS)
    add_library(${target}_clap_impl STATIC "${fw}/src/formats/clap/clap_plugin.cpp")
    target_link_libraries(${target}_clap_impl PUBLIC ${target}_lib clap)
    set(formats CLAP)
    foreach(f VST3 AUV2)
      if(f IN_LIST HOLLOW_FORMATS)
        list(APPEND formats ${f})
      endif()
    endforeach()
    set(standalone "")
    if("STANDALONE" IN_LIST HOLLOW_FORMATS)
      set(standalone STANDALONE_CONFIGURATIONS standalone "${P_NAME}" "${P_BUNDLE_ID}")
    endif()
    make_clapfirst_plugins(
      TARGET_NAME ${target}
      IMPL_TARGET ${target}_clap_impl
      OUTPUT_NAME "${P_NAME}"
      ENTRY_SOURCE "${fw}/src/formats/clap/entry.cpp"
      BUNDLE_IDENTIFIER "${P_BUNDLE_ID}"
      BUNDLE_VERSION "${P_VERSION}"
      COPY_AFTER_BUILD FALSE
      WINDOWS_FOLDER_VST3 TRUE
      ASSET_OUTPUT_DIRECTORY "${out}"
      PLUGIN_FORMATS ${formats}
      AUV2_MANUFACTURER_NAME "${P_VENDOR}"
      AUV2_MANUFACTURER_CODE "${P_AU_MANUFACTURER}"
      AUV2_SUBTYPE_CODE "${P_AU_SUBTYPE}"
      AUV2_INSTRUMENT_TYPE "${P_AU_TYPE}"
      ${standalone})
    # Visual Studio 18's MSVC 14.51 makes <experimental/coroutine>, which clap-wrapper's Windows
    # standalone reaches through C++/WinRT, a hard error unless this is defined.
    if(MSVC)
      foreach(t ${target}_standalone ${target}_standalone-clap-wrapper-standalone-lib)
        if(TARGET ${t})
          target_compile_definitions(${t} PRIVATE _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)
        endif()
      endforeach()
    endif()
  endif()

  if("VST2" IN_LIST HOLLOW_FORMATS)   # with the DXi on 32-bit Windows, a .vst bundle on macOS
    set(vst2 ${target}_vst2)
    add_library(${vst2} MODULE "${fw}/src/formats/vst2/vst2.cpp")
    target_include_directories(${vst2} PRIVATE "${fw}/third_party/vst2")
    target_link_libraries(${vst2} PRIVATE ${target}_lib)
    set_target_properties(${vst2} PROPERTIES OUTPUT_NAME "${P_NAME}" PREFIX "" LIBRARY_OUTPUT_DIRECTORY "${out}/VST2")

    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4 AND "DXI" IN_LIST HOLLOW_FORMATS)
      # The DXi lives in the 32-bit VST2 DLL and drives its AEffect. Microsoft's DirectShow
      # BaseClasses (MIT) carry the filter plumbing; they predate /permissive- and /W4.
      if(NOT TARGET hollow_strmbase)
        file(GLOB strmbase "${fw}/third_party/strmbase/*.cpp")
        add_library(hollow_strmbase STATIC ${strmbase})
        target_include_directories(hollow_strmbase PUBLIC "${fw}/third_party/strmbase")
        target_compile_definitions(hollow_strmbase PUBLIC UNICODE _UNICODE)
        target_compile_options(hollow_strmbase PRIVATE /W0)
        target_link_libraries(hollow_strmbase PUBLIC strmiids winmm ole32 oleaut32 uuid advapi32 user32 gdi32)
      endif()
      if(P_DXI_CLSID)
        string(REGEX REPLACE "[{}]" "" clsid "${P_DXI_CLSID}")
      else()
        string(UUID clsid NAMESPACE 6ba7b811-9dad-11d1-80b4-00c04fd430c8 NAME "hollow-dxi:${P_BUNDLE_ID}" TYPE SHA1)
      endif()
      string(UUID page NAMESPACE "${clsid}" NAME "page" TYPE SHA1)
      foreach(id clsid page)   # "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" -> a GUID initializer
        string(REGEX REPLACE "^(........)-(....)-(....)-(....)-(............)$" "\\1;\\2;\\3;\\4\\5" parts "${${id}}")
        list(GET parts 0 a)
        list(GET parts 1 b)
        list(GET parts 2 c)
        list(GET parts 3 d)
        string(REGEX REPLACE "(..)" "0x\\1," d "${d}")
        string(REGEX REPLACE ",$" "" d "${d}")
        set(${id} "{0x${a},0x${b},0x${c},{${d}}}")
      endforeach()
      target_sources(${vst2} PRIVATE "${fw}/src/formats/dxi/dxi.cpp")
      target_compile_definitions(${vst2} PRIVATE "HOLLOW_DXI_CLSID=${clsid}" "HOLLOW_DXI_PAGE_CLSID=${page}")
      target_link_libraries(${vst2} PRIVATE hollow_strmbase)
    endif()

    if(APPLE)
      # A .vst bundle. The Info.plist is written here; CMake's default template is for apps.
      set(plist "${CMAKE_CURRENT_BINARY_DIR}/${vst2}_Info.plist")
      file(WRITE "${plist}" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\"><dict>
<key>CFBundleDevelopmentRegion</key><string>English</string>
<key>CFBundleExecutable</key><string>${P_NAME}</string>
<key>CFBundleIdentifier</key><string>${P_BUNDLE_ID}.vst</string>
<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
<key>CFBundleName</key><string>${P_NAME}</string>
<key>CFBundlePackageType</key><string>BNDL</string>
<key>CFBundleSignature</key><string>????</string>
<key>CFBundleShortVersionString</key><string>${P_VERSION}</string>
<key>CFBundleVersion</key><string>${P_VERSION}</string>
<key>LSMinimumSystemVersion</key><string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>
</dict></plist>
")
      file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${vst2}_PkgInfo" "BNDL????")
      set_target_properties(${vst2} PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION vst MACOSX_BUNDLE_INFO_PLIST "${plist}")
      add_custom_command(TARGET ${vst2} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/${vst2}_PkgInfo" "$<TARGET_BUNDLE_CONTENT_DIR:${vst2}>/PkgInfo"
        COMMAND codesign -s - --force "$<TARGET_BUNDLE_DIR:${vst2}>"
        VERBATIM)
    endif()
  endif()

  # Every format has the same file name, so on Windows their import libraries (.lib/.exp) would
  # overwrite each other in a parallel build; give each its own folder.
  foreach(t clap vst3 standalone vst2)
    if(TARGET ${target}_${t})
      set_target_properties(${target}_${t} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/implib/${t}")
    endif()
  endforeach()

  # BIN: each format into a folder of its own, a bundle as its whole folder. A VST3 is a folder everywhere
  # (clap-wrapper puts the binary two levels inside it off macOS too).
  if(P_BIN)
    set(vst2dir VST2)
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
      set(vst2dir "VST2 32-bit and DXi")
    endif()
    foreach(pair "clap|CLAP" "vst3|VST3" "auv2|AU" "standalone|Standalone" "vst2|${vst2dir}")
      string(REPLACE "|" ";" pair "${pair}")
      list(GET pair 0 t)
      list(GET pair 1 dir)
      set(tgt ${target}_${t})
      if(NOT TARGET ${tgt})
        continue()
      endif()
      get_target_property(bundle ${tgt} BUNDLE)
      get_target_property(appBundle ${tgt} MACOSX_BUNDLE)
      if(APPLE AND (bundle OR appBundle))
        set(from "$<TARGET_BUNDLE_DIR:${tgt}>")
        set(to "${P_BIN}/${dir}/$<TARGET_BUNDLE_DIR_NAME:${tgt}>")
        set(copy copy_directory)
      elseif(t STREQUAL "vst3")
        set(from "$<TARGET_FILE_DIR:${tgt}>/../..")
        set(to "${P_BIN}/${dir}/${P_NAME}.vst3")
        set(copy copy_directory)
      else()
        set(from "$<TARGET_FILE:${tgt}>")
        set(to "${P_BIN}/${dir}/")
        set(copy copy)
      endif()
      set(clear "")
      if(copy STREQUAL "copy_directory")   # a bundle is replaced, not merged: no file of an older build stays in it
        set(clear COMMAND "${CMAKE_COMMAND}" -E rm -rf "${to}")
      endif()
      add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${P_BIN}/${dir}"
        ${clear}
        COMMAND "${CMAKE_COMMAND}" -E ${copy} "${from}" "${to}"
        VERBATIM)
    endforeach()
  endif()
endfunction()
