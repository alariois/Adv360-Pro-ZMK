# Compiled into the ZMK app target so <zmk/...> headers are available.
if(CONFIG_ZMK_KEY_MONITOR)
  target_sources(app PRIVATE src/key_monitor.c)
endif()
