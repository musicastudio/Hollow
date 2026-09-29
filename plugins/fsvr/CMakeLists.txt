# The skin is FSVR's own, in the FSVR repository beside this one (FSVR_DIR names another).
set(FSVR_DIR "${CMAKE_SOURCE_DIR}/../FSVR" CACHE PATH "The FSVR repository, whose plugin/skin this shell embeds")
set(icons "")
if(EXISTS "${FSVR_DIR}/icon.ico")   # FSVR's own icon, when its checkout has one
  set(icons WINDOWS_ICON "${FSVR_DIR}/icon.ico" MACOS_ICON "${FSVR_DIR}/icon.icns")
endif()
hollow_add_plugin(fsvr
  NAME "FSVR"
  SKIN "${FSVR_DIR}/plugin/skin"
  SOURCES plugin.cpp
  BUNDLE_ID studio.musica.fsvr-hollow
  VERSION 0.1.0
  VENDOR musica.studio
  AU_MANUFACTURER Msca
  AU_SUBTYPE FsvH
  ${icons})
