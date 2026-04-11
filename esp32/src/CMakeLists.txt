FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include"
    REQUIRES
        esp_tinyusb
        fatfs
        nvs_flash
        esp_wifi
        esp_netif
        esp_http_server
        esp-tls
        driver
        sdmmc
        vfs
)
