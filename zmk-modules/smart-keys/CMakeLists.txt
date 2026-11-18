# Compiled into the ZMK app target so <zmk/...> headers are available.
# (Module sources also link before ZMK's own listeners, so their event
# listeners run first - smart-keys relies on that.)
target_sources_ifdef(CONFIG_ZMK_MOD_MORPH_UNMASK_ON_NEXT_KEY app PRIVATE src/mod_morph_unmask.c)
