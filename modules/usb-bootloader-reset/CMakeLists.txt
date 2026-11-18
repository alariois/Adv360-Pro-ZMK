if(CONFIG_ZMK_USB_BOOTLOADER_RESET)
  zephyr_library()
  zephyr_library_sources(src/usb_bootloader_reset.c)
endif()
