idf_component_register(SRCS "esp_lcd_st7102.c" "esp_lcd_st7102_general.c" "esp_lcd_st7102_rgb.c" "esp_lcd_st7102_mipi.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       REQUIRES "esp_lcd" "driver")