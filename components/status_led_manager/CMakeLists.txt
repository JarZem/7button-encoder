idf_component_register(
    SRCS "status_led_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer espressif__led_strip
)
