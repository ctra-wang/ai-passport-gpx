idf_component_register(
    SRCS "main.c"
         "route_app.c"
         "route_audio.c"
         "route_ble.c"
         "route_format.c"
         "route_nav.c"
         "route_storage.c"
         "route_ui.c"
         "route_ui_geometry.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_timer nvs_flash spi_flash
)

target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/noto_sans_sc_14.c")
