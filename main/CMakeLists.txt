idf_component_register(SRCS "main.c" "audio.c" "synth.c" "usb_midi.c" "web.c" "fx.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "index.html"
                    PRIV_REQUIRES usb esp_driver_i2s esp_driver_gpio
                                  esp_http_server esp_wifi esp_netif esp_event
                                  nvs_flash json)
