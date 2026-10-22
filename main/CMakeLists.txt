idf_component_register(
    SRCS "main.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             fatfs wear_levelling esp_http_client json
    PRIV_REQUIRES mbedtls efuse
)

# 数字工牌(badge)模块源文件。
target_sources(${COMPONENT_LIB} PRIVATE
    "badge_state.c"
    "badge_fmt.c"
    "badge_blufi_security.c"
    "badge_wifi.c"
    "badge_store.c"
    "badge_tasks.c"
    "badge_upload.c"
    "badge_record.c"
    "badge_ui.c"
)

# 中文字体(GB2312 一级字库 16px + 静态文案 20px),由 lv_font_conv 生成,
# 位于仓库根 assets/fonts,经 LV_FONT_DECLARE 在 badge_ui.c 中引用。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/badge_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/badge_font_20.c"
)
