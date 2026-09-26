option(PJOURNAL_IME_ENABLE_PINYIN "Build the pinyin IME module" ON)
option(PJOURNAL_IME_ENABLE_LIANGFEN "Embed and build the liangfen IME module" ON)
option(PJOURNAL_IME_ENABLE_WUBI "Build optional wubi IME support" OFF)
option(PJOURNAL_IME_ENABLE_SHUANGPIN "Build optional shuangpin IME support" OFF)
option(PJOURNAL_IME_PERF_LOG "Enable slow IME lookup timing logs" OFF)

set(PJOURNAL_EMBED_FILES
    "ime/ime_table_pinyin.bin"
    "ime/english_words.txt"
    "terminus22.fnt"
    "fontlibrary18.fnt"
)

if(PJOURNAL_IME_ENABLE_LIANGFEN)
    list(APPEND PJOURNAL_EMBED_FILES "ime/liangfen.bin")
endif()

idf_component_register(SRCS
    "main.cpp"
    "pjournal_app.cpp"
    "ui_helpers.cpp"
    "safe_file.cpp"
    "screen_editor.cpp"
    "screen_settings.cpp"
    "quick_edit.cpp"
    "typing_click.cpp"
    "click_samples.cpp"
    "screen_bt_manage.cpp"
    "screen_file_manager.cpp"
    "file_manager_server.cpp"
    "font_renderer.cpp"
    "bt_keyboard.cpp"
    "journal_storage.cpp"
    "settings_manager.cpp"
    "wifi_manager.cpp"
    "webdav_client.cpp"
    "flomo_client.cpp"
    "deepseek_client.cpp"
    "pcf85063.cpp"
    "json_parser.cpp"
    "json_utils.cpp"
    "ime/IME.cpp"
    "ime/yong_dict.cpp"
    "ime/yong_pinyin.cpp"
    "screen_gtd.cpp"
    "screen_outline.cpp"
    "screen_inspiration.cpp"
    "screen_voice.cpp"
    "screen_polish.cpp"
    "screen_polish_prompt.cpp"
    "voice_input.cpp"
    "markdown_render.cpp"
    "vertical_layout.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES
        ${PJOURNAL_EMBED_FILES}
    PRIV_REQUIRES
        spi_flash
        nvs_flash
        esp_wifi
        esp_netif
        esp-tls
        esp_http_client
        esp_http_server
        esp_event
        bt
        esp_hid
        lwip
        mbedtls
        fatfs
        sdmmc
        driver
        u8g2
        u8g2_st7305
        esp_adc
        esp_websocket_client
        esp_codec_dev
        esp_audio_codec
        efuse
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE PJOURNAL_VERSION="4.8.2")
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PJOURNAL_IME_ENABLE_PINYIN=$<BOOL:${PJOURNAL_IME_ENABLE_PINYIN}>
    PJOURNAL_IME_ENABLE_LIANGFEN=$<BOOL:${PJOURNAL_IME_ENABLE_LIANGFEN}>
    PJOURNAL_IME_ENABLE_WUBI=$<BOOL:${PJOURNAL_IME_ENABLE_WUBI}>
    PJOURNAL_IME_ENABLE_SHUANGPIN=$<BOOL:${PJOURNAL_IME_ENABLE_SHUANGPIN}>
    PJOURNAL_IME_PERF_LOG=$<BOOL:${PJOURNAL_IME_PERF_LOG}>
)
