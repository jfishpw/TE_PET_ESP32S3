/*
 * libopus 嵌入式编译配置（ESP32-S3 / Xtensa）
 * - FIXED_POINT：不依赖 FPU，符号/数值路径固定，稳定性好
 * - DISABLE_FLOAT_API：只保留整数 opus_encode/opus_decode（PCM 为 16 位）
 * - VAR_ARRAYS：临时帧走任务栈 VLA（线程安全，多编解码器互不干扰）。
 *   调用 opus 解码/编码的任务需配足栈（见 xz_task 改为 20KB）。
 *   不用 NONTHREADSAFE_PSEUDOSTACK：其 120KB 堆 scratch 对内 RAM 压力大。
 */
#ifndef OPUS_CONFIG_H
#define OPUS_CONFIG_H

#define OPUS_BUILD 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_INTTYPES_H 1

#define FIXED_POINT 1
#define DISABLE_FLOAT_API 1
#define VAR_ARRAYS 1

#endif /* OPUS_CONFIG_H */