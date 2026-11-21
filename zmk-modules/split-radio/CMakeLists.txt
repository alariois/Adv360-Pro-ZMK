# Compiled into the ZMK app target so <zmk/...> headers are available.
if(CONFIG_ZMK_SPLIT_RADIO)
  target_sources(app PRIVATE src/split_radio.c src/ccm.c)
endif()
