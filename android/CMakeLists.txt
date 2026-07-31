# CMakeLists.txt для libamplitude.so (JNI-обёртка).
#
# Годится и для репозитория как есть (android/ + src/ рядом), и для варианта,
# когда все .cpp/.h скопированы в одну папку app/src/main/cpp/.

cmake_minimum_required(VERSION 3.22.1)
project(amplitude LANGUAGES CXX)

# --- где лежат исходники ядра -------------------------------------------
get_filename_component(AMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if (EXISTS "${AMP_ROOT}/src/decoder.h")
    set(AMP_SRC "${AMP_ROOT}/src")                 # структура репозитория
elseif (EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/decoder.h")
    set(AMP_SRC "${CMAKE_CURRENT_LIST_DIR}/src")
else ()
    set(AMP_SRC "${CMAKE_CURRENT_LIST_DIR}")       # всё в одной папке cpp/
endif ()
message(STATUS "amplitude: исходники ядра — ${AMP_SRC}")

if (NOT EXISTS "${AMP_SRC}/minimp3_ex.h")
    message(FATAL_ERROR
            "Нет ${AMP_SRC}/minimp3_ex.h. Возьмите minimp3.h и minimp3_ex.h из "
            "https://github.com/lieff/minimp3 (или выполните `make deps` в корне проекта).")
endif ()

add_library(amplitude SHARED
        ${CMAKE_CURRENT_LIST_DIR}/amplitude_jni.cpp
        ${AMP_SRC}/decoder_open.cpp
        ${AMP_SRC}/decoder_minimp3.cpp
        ${AMP_SRC}/decoder_mediandk.cpp)

target_include_directories(amplitude PRIVATE ${AMP_SRC})
target_compile_features(amplitude PRIVATE cxx_std_11)

# Исключения оставлены включёнными: обработчик JNI ловит std::bad_alloc
# и превращает его в OutOfMemoryError вместо аварийного завершения процесса.
target_compile_options(amplitude PRIVATE
        -O3
        -fno-rtti
        -ffunction-sections
        -fdata-sections
        -Wall -Wextra)

if (ANDROID_ABI STREQUAL "armeabi-v7a")
    target_compile_options(amplitude PRIVATE -mfpu=neon -mfloat-abi=softfp)
endif ()

# libmediandk — системный декодер (AAC/M4A, FLAC, OGG, WAV). Есть с API 21,
# AMediaDataSource (декодирование из byte[]) — с API 28.
target_link_libraries(amplitude PRIVATE mediandk)

target_link_options(amplitude PRIVATE
        -Wl,--gc-sections
        # совместимость с устройствами с 16 КБ страницами (Android 15+)
        -Wl,-z,max-page-size=16384)
