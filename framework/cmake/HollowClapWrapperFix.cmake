# clap-wrapper's VST3 controller hands an edit made in the plug-in's own editor to the host with
# performEdit() but never stores it in its own parameter, so getParamNormalized() goes on returning the
# value from before the edit until the plug-in asks for a rescan. Cubase reads the controller around an
# edit and sends what it read back to the processor: a choice made in the editor (a performance, a
# voice bank) jumped back to the one before. A VST3 editor sets the controller's value first and then
# calls performEdit(), and this makes the wrapper do the same (wrapasvst3.cpp, ClapAsVst3::onIdle).
# v0.16.0 and the next-branch commit pinned above (527a374) both have it unchanged; drop this patch
# once a release stores the value itself.
#
# Its VST3 pitch bend parameter (one per MIDI channel, mapped to 0xE0) defaults to 0, which is a full bend
# down: a host that sets the wrapper's parameters to their defaults played every note bent (detail/vst3/
# parameter.cpp). The default is the centre, 8192 of 16383, which is no bend.
#
# The Windows standalone with ASIO also answers the system command 0x100 by opening the ASIO driver's own
# control panel (ASIOControlPanel in the ASIO SDK RtAudio builds in, a no-op while no ASIO driver is
# loaded), which Hollow's platformDriverPanel posts. clap-wrapper has no way to reach it.
#
# One-line replaces rather than a patch file: Windows checkouts with core.autocrlf turn the fetched
# source into CRLF, which a patch's context lines would not match. Runs on every configure, idempotent.
function(hollow_patch_clap_wrapper file old new what)
  file(READ "${file}" src)
  string(FIND "${src}" "${new}" done)
  if(NOT done EQUAL -1)
    return()
  endif()
  string(FIND "${src}" "${old}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "clap-wrapper changed: ${file} has no '${old}' to patch. Check whether the new "
                        "version still needs ${what}, and drop or update ${CMAKE_CURRENT_FUNCTION_LIST_FILE}.")
  endif()
  string(REPLACE "${old}" "${new}" src "${src}")
  file(WRITE "${file}" "${src}")
  message(STATUS "hollow: clap-wrapper ${what} (${file})")
endfunction()

function(hollow_fix_clap_wrapper dir)
  hollow_patch_clap_wrapper("${dir}/src/wrapasvst3.cpp"
    "performEdit(param->getInfo().id, param->asVst3Value(v));"
    "if (param) { param->setNormalized(param->asVst3Value(v)); performEdit(param->getInfo().id, param->asVst3Value(v)); }   // hollow: the controller holds the value before the host hears of it"
    "VST3 controller stores editor edits")
  hollow_patch_clap_wrapper("${dir}/src/detail/vst3/parameter.cpp"
    "v.stepCount = 16383;"
    "v.stepCount = 16383; v.defaultNormalizedValue = 8192.5 / 16383;   // hollow: pitch bend rests at the centre, 8192 once the wrapper truncates it"
    "VST3 pitch bend defaults to the centre")
  if(HOLLOW_STANDALONE_ASIO)
    set(file "${dir}/src/detail/standalone/windows/windows_standalone.cpp")
    hollow_patch_clap_wrapper("${file}" "#include \"windows_standalone.h\""
      "#include \"windows_standalone.h\"\nlong ASIOControlPanel();   // hollow: the ASIO SDK's, which RtAudio builds in"
      "standalone declares ASIOControlPanel")
    hollow_patch_clap_wrapper("${file}" "case Menu::Identifier::AudioMidiSettings:"
      "case 0x100: ASIOControlPanel(); return 0;   // hollow: the ASIO driver's own panel (platformDriverPanel)\n                 case Menu::Identifier::AudioMidiSettings:"
      "standalone opens the ASIO driver's panel")
  endif()
endfunction()
