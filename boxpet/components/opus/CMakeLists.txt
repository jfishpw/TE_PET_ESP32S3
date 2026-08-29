# 本地 libopus 组件（fixed-point，整数 API）
# 源码来自 libopus v1.5.2，仅选取 celt/silk/src 与配置相关文件

file(GLOB OPUS_CELT_SRCS lib/celt/*.c)
file(GLOB OPUS_SILK_SRCS lib/silk/*.c)
file(GLOB OPUS_SILK_FIXED_SRCS lib/silk/fixed/*.c)

idf_component_register(
    SRCS
        lib/src/opus.c
        lib/src/opus_encoder.c
        lib/src/opus_decoder.c
        lib/src/repacketizer.c
        lib/src/extensions.c
        ${OPUS_CELT_SRCS}
        ${OPUS_SILK_SRCS}
        ${OPUS_SILK_FIXED_SRCS}
    INCLUDE_DIRS
        "."
        "lib/include"
        "lib/celt"
        "lib/silk"
        "lib/silk/fixed"
        "lib/silk/float"
        "lib/src"
    REQUIRES log
)

# 让 src/celt/silk 在 #ifdef HAVE_CONFIG_H 时能 include "config.h"（fixed-point 等）
target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_CONFIG_H)

# ESP-IDF 默认打开过度告警，这里关掉 libopus 常见告警以免刷屏
if(CMAKE_C_COMPILER_ID MATCHES "(GNU|Clang)")
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-sign-compare
        -Wno-array-bounds
        -Wno-stringop-overflow
    )
endif()