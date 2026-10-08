idf_component_register(
    SRCS "main.c" "assets.c" "config.c" "dmx_out.c" "dmx_buffer.c" "artnet.c" "sacn.c"
         "wifi_mgr.c" "mdns_svc.c" "captive_dns.c" "web.c" "cli.c" "console.c" "names.c" "scenes.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html" "web/index.css" "web/index.js"
                   "web/console.html" "web/console.css" "web/console.js"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server
                  esp_driver_uart esp_driver_gpio esp_app_format app_update console lwip esp_timer esp_partition
)
