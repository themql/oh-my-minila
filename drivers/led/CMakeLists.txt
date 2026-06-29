zephyr_library()

zephyr_library_sources_ifdef(CONFIG_OMM_LED_TEST omm_led_test.c)
