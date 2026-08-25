# BoxPet — ESP32 电子宠物（类拓麻歌子）
# 目标硬件：正点原子 ESP32 AI BOX0（ATK-DNESP32S3B0）
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(boxpet)

# Suppress -Wmissing-field-initializers so esp_timer_create_args_t
# designated initializers can omit .arg / .arg and similar optional fields.
idf_build_set_property(GLOBAL_COMPILE_OPTIONS "-Wno-error=missing-field-initializers")