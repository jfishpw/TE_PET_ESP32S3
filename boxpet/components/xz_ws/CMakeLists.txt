# xz_ws — 轻量 WebSocket 客户端（RFC6455，客户端 mask，支持 ws/wss）
idf_component_register(
    SRCS "xz_ws.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp-tls mbedtls esp_timer
)