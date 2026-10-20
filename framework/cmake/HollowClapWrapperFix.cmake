# clap-wrapper's VST3 controller hands an edit made in the plug-in's own editor to the host with
# performEdit() but never stores it in its own parameter, so getParamNormalized() goes on returning the
# value from before the edit until the plug-in asks for a rescan. Cubase reads the controller around an
# edit and sends what it read back to the processor: a choice made in the editor (a performance, a
# voice bank) jumped back to the one before. A VST3 editor sets the controller's value first and then
# calls performEdit(), and this makes the wrapper do the same (wrapasvst3.cpp, ClapAsVst3::onIdle).
# v0.17.0 still has it unchanged; drop this patch once a release stores the value itself.
#
# Its VST3 pitch bend parameter (one per MIDI channel, mapped to 0xE0) defaults to 0, which is a full bend
# down: a host that sets the wrapper's parameters to their defaults played every note bent (detail/vst3/
# parameter.cpp). The default is the centre, 8192 of 16383, which is no bend. Still so in v0.17.0.
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
endfunction()
