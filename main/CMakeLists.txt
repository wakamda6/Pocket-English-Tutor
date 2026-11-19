idf_component_register(
    SRCS "english_tutor.c"
         "tutor_session.c"
         "tutor_pcm.c"
         "tutor_adpcm.c"
         "tutor_cloud_message.c"
         "tutor_setup.c"
         "tutor_setup_form.c"
         "tutor_setup_dns.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bsp esp_event esp_http_server esp_netif esp_timer lwip
                  esp_wifi json mbedtls nvs_flash
)

target_sources(${COMPONENT_LIB} PRIVATE
    "../assets/fonts/tutor_ui_font_16.c"
    "../assets/fonts/tutor_ui_font_20.c"
)
