file(GLOB_RECURSE srcs *.c)
 
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS .
    REQUIRES lvgl spiffs esp_littlefs
)