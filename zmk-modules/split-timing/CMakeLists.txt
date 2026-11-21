# Compiled into the ZMK app target so <zmk/...> headers are available.
# The central's listener must run before every other position listener
# (key-monitor, smart-keys, ZMK's own): list this module FIRST in
# EXTRA_MODULES (listeners run in link order).
if(CONFIG_ZMK_SPLIT_LINK_2M_PHY OR CONFIG_ZMK_SPLIT_LINK_NO_ADVERTISING OR CONFIG_ZMK_SPLIT_LINK_NO_ADV_ON_USB)
  target_sources(app PRIVATE src/link_tuning.c)
endif()

zephyr_include_directories(include)
if(CONFIG_ZMK_SPLIT_TIMING)
  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE src/central.c)
  elseif(CONFIG_ZMK_SPLIT_BLE)
    target_sources(app PRIVATE src/peripheral.c)
  endif()
endif()
