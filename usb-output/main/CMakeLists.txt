set(srcs
    "main.c"
    "macpass_hid.c"
    "macpass_spi.c"
    "macpass_usb.c"
    "macpass_macro.c"
    "macro_defaults.c"
    "macro_profile.c"
    "macro_web.c"
    "perf_log.c"
    )

if(CONFIG_MACRO_WEB_UI)
    if(CONFIG_HTTPD_WS_SUPPORT)
        list(APPEND srcs "macro_ws.c")
    else()
        list(APPEND srcs "macro_ws_stub.c")
    endif()
endif()

# Always link Wi-Fi / HTTP / JSON / SPIFFS for main. Kconfig still gates *runtime* use with
# CONFIG_MACRO_WEB_UI; conditional PRIV_REQUIRES in CMake was unreliable here (preprocessor on,
# CMake deps off), which broke includes for esp_event, cJSON, etc.
set(priv_reqs
    esp_driver_gpio
    esp_timer
    esp_wifi
    esp_netif
    nvs_flash
    esp_event
    esp_http_server
    esp_partition
    spiffs
    json
    )

if(NOT CMAKE_SCRIPT_MODE_FILE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    set(doc_src_dir "${CMAKE_CURRENT_LIST_DIR}/../docs")
    set(doc_gen_script "${CMAKE_CURRENT_LIST_DIR}/../tools/generate_doc_assets.py")
    set(doc_header "${CMAKE_CURRENT_LIST_DIR}/doc_assets.h")

    add_custom_command(
        OUTPUT "${doc_header}"
        COMMAND "${Python3_EXECUTABLE}" "${doc_gen_script}" "${doc_src_dir}" "${doc_header}"
        DEPENDS
            "${doc_gen_script}"
            "${CMAKE_CURRENT_LIST_DIR}/web/index.html"
            "${doc_src_dir}/QUICK_WALKTHROUGH.md"
            "${doc_src_dir}/PROFILE_SCHEMA.md"
            "${doc_src_dir}/MACRO_STRUCTURE_DETAILED_GUIDE.md"
            "${doc_src_dir}/MACRO_WEB_UI_PLAN.md"
            "${doc_src_dir}/README.md"
        COMMENT "Generating doc_assets.h from docs/*.md"
        VERBATIM
    )

    add_custom_target(generate_doc_assets DEPENDS "${doc_header}")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${priv_reqs}
    )

if(TARGET generate_doc_assets)
    add_dependencies(${COMPONENT_LIB} generate_doc_assets)
endif()
