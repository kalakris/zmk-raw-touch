# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_RAW_TOUCH OR CONFIG_ZMK_INPUT_PROCESSOR_RAW_TOUCH_SCROLL OR CONFIG_ZMK_INPUT_PROCESSOR_RAW_TOUCH_IDLE_FILTER)
  zephyr_include_directories(include)

  zephyr_library()
  zephyr_library_sources_ifdef(CONFIG_ZMK_RAW_TOUCH src/hid.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_RAW_TOUCH src/raw_touch.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_RAW_TOUCH src/endpoints.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_RAW_TOUCH_USB src/usb_hid.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_RAW_TOUCH_BLE src/hog.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_RAW_TOUCH_SCROLL
                               src/input_processor_raw_touch_scroll.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_RAW_TOUCH_IDLE_FILTER
                               src/input_processor_raw_touch_idle_filter.c)
endif()
