# The skin is FSVR's own, in the FSVR repository beside this one (FSVR_DIR names another).
set(FSVR_DIR "${CMAKE_SOURCE_DIR}/../FSVR" CACHE PATH "The FSVR repository, whose plugin/skin this shell embeds")
hollow_add_plugin(fsvr
  NAME "FSVR"
  SKIN "${FSVR_DIR}/plugin/skin"
  SOURCES plugin.cpp
  BUNDLE_ID studio.musica.fsvr-hollow
  VERSION 0.1.0
  VENDOR musica.studio
  AU_MANUFACTURER Msca
  AU_SUBTYPE FsvH)
