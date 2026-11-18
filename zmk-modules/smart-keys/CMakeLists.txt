# Compiled into the ZMK app target so <zmk/...> headers are available.
# (Module sources also link before ZMK's own objects, so their event
# listeners run first - smart-keys relies on that.)
zephyr_include_directories(include)
target_sources_ifdef(CONFIG_ZMK_MOD_MORPH_UNMASK_ON_NEXT_KEY app PRIVATE src/mod_morph_unmask.c)

if(CONFIG_ZMK_BEHAVIOR_SMART_REPEAT)
  target_sources(app PRIVATE src/smart_repeat.c src/masked_mods.c)
  # masked_mods.c observes mod-morph masking (hid.c keeps it private)
  zephyr_ld_options(-Wl,--wrap=zmk_hid_masked_modifiers_set
                    -Wl,--wrap=zmk_hid_masked_modifiers_clear)
endif()

target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_ONESHOT_LAYER app PRIVATE src/oneshot_layer.c)
