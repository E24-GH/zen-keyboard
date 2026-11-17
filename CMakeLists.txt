if(CONFIG_SHIELD_ZEN_LEFT OR CONFIG_SHIELD_ZEN_RIGHT)
  zephyr_library()
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/drivers/iqs7211e/include)
  # zmk/app's own include dir (drivers/input_processor.h, zmk/*.h, ...) isn't
  # inherited automatically by out-of-tree modules.
  zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_library_sources(src/board.c src/mini_trackpad_init_reg.c src/zen_pointer_settings.c src/zen_config_codec.c
                         src/zen_pointer_rotate.c
                         drivers/zen_input_processor/src/zen_input_processor_live_scale.c
                         drivers/zen_input_processor/src/zen_input_processor_auto_mouse.c)
  # Both halves: Zephyr's default fatal handler spins forever with interrupts
  # locked and there's no watchdog, so a faulting half stays dead until someone
  # cycles its power switch. See the comment at the top of the file.
  zephyr_library_sources(src/zen_fault_recovery.c)
  # Only where ZMK actually owns the USB stack. On a split peripheral
  # CONFIG_ZMK_USB is off (zmk/app/CMakeLists.txt builds the USB/HID/endpoint
  # layer for central + standalone only), so the CDC-ACM port the bridge polls
  # exists but nothing in ZMK enables or manages it -- and polling it there
  # stops the peripheral coming back from deep sleep. See the comment at the
  # top of src/zen_config_serial_bridge.c.
  zephyr_library_sources_ifdef(CONFIG_ZMK_USB src/zen_config_serial_bridge.c)
  zephyr_library_sources_ifdef(CONFIG_PMW3610_ALT drivers/pmw3610_alt/src/pmw3610.c)
  zephyr_library_sources_ifdef(CONFIG_IQS7211E drivers/iqs7211e/src/iqs7211e.c drivers/iqs7211e/src/iqs7211e_reg.c)
endif()
