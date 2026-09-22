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
         "cook_model.c"
         "cook_settings.c"
         "cook_power.c"
         "demo_cook.c"
         "windmill_model.c"
         "demo_windmill.c"
         "generated/cook_data.c"
         "generated/cook_covers.c"
    INCLUDE_DIRS "." "generated" "assets/windmill"
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
    # 菜谱封面:打包阶段烘焙好的两档 JPEG,运行时由 LVGL 内置 tjpgd 直接解码。
    # 全部封面拼成一个 blob,而不是上百个嵌入文件,避免构建系统被几百个目标拖慢。
    EMBED_FILES "assets/covers_large.bin"
                "assets/covers_small.bin"
)

# 中文字集字体。由 tools/build_cook_fonts.py 生成(Noto Sans SC, SIL OFL 1.1),
# 只收录 assets/fonts/cook_chars.txt 列出的字形,避免整包 CJK 字库占满 Flash。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/cook_font_14.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/cook_font_18.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/cook_font_26.c"
)

# 吹气转风车素材:windmill/scene 的 PNG 预处理成的 ARGB8888 常量数组(见 _dev/gen_windmill_imgs.py)。
# 含透明通道的图层(JPEG 无透明)只能走这条路径,运行时直接 lv_image_set_src,无需解码器。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/assets/windmill/windmill_static.c"
    "${CMAKE_CURRENT_LIST_DIR}/assets/windmill/windmill_rotor.c"
    "${CMAKE_CURRENT_LIST_DIR}/assets/windmill/windmill_flowers.c"
    "${CMAKE_CURRENT_LIST_DIR}/assets/windmill/windmill_cloud1.c"
    "${CMAKE_CURRENT_LIST_DIR}/assets/windmill/windmill_cloud2.c"
    "${CMAKE_CURRENT_LIST_DIR}/assets/windmill/windmill_cloud4.c"
    "${CMAKE_CURRENT_LIST_DIR}/assets/windmill/windmill_sea.c"
    "${CMAKE_CURRENT_LIST_DIR}/assets/windmill/windmill_grass.c"
)
