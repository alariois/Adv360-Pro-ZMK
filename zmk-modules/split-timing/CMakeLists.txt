# Compiled into the ZMK app target so <zmk/...> headers are available.
# The central's listener must run before every other position listener
# (key-monitor, smart-keys, ZMK's own): list this module FIRST in
# EXTRA_MODULES (listeners run in link order).
if(CONFIG_ZMK_SPLIT_TIMING)
  zephyr_include_directories(include)
  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE src/central.c)
  else()
    target_sources(app PRIVATE src/peripheral.c)
  endif()
endif()
