idf_component_register(
    SRCS "main.c"
         "route_app.c"
         "route_audio.c"
         "route_ble.c"
         "route_format.c"
         "route_nav.c"
         "route_storage.c"
         "route_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_timer nvs_flash spi_flash
)
