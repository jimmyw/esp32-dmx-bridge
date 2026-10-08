idf_component_register(
    SRCS "main.c" "assets.c" "config.c" "dmx_out.c" "dmx_buffer.c" "artnet.c" "sacn.c"
         "wifi_mgr.c" "mdns_svc.c" "captive_dns.c" "web.c" "cli.c" "console.c" "names.c" "scenes.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server
                  esp_driver_uart esp_driver_gpio esp_app_format app_update console lwip esp_timer esp_partition
)

# Web UI: webpack bundles web/src into one self-contained, gzipped HTML file per page
# (see web/webpack.config.js). Needs Node.js/npm; build tools are installed with "npm ci".
find_program(NPM npm REQUIRED)
set(web_dir "${PROJECT_DIR}/web")
set(web_out "${CMAKE_CURRENT_BINARY_DIR}/web")
set(web_pages index console)
file(GLOB web_srcs CONFIGURE_DEPENDS "${web_dir}/src/*")
list(TRANSFORM web_pages APPEND ".html.gz" OUTPUT_VARIABLE web_gz)
list(TRANSFORM web_gz PREPEND "${web_out}/")

add_custom_command(OUTPUT "${web_dir}/node_modules/.package-lock.json"
    COMMAND "${NPM}" ci --no-audit --no-fund
    WORKING_DIRECTORY "${web_dir}"
    DEPENDS "${web_dir}/package.json" "${web_dir}/package-lock.json"
    COMMENT "Installing web UI build tools (npm ci)"
    VERBATIM)
add_custom_command(OUTPUT ${web_gz}
    COMMAND "${NPM}" run build -- --env "outDir=${web_out}"
    WORKING_DIRECTORY "${web_dir}"
    DEPENDS ${web_srcs} "${web_dir}/webpack.config.js" "${web_dir}/node_modules/.package-lock.json"
    COMMENT "Bundling web UI (webpack)"
    VERBATIM)
foreach(gz ${web_gz})
    target_add_binary_data(${COMPONENT_LIB} "${gz}" BINARY)
endforeach()
