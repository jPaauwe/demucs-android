# Demucs Android engine: music source separation (Demucs v4 htdemucs_6s) for
# Android phones, in C++ on every CPU core and the GPU (Vulkan).
#
# In your project:
#   add_subdirectory(demucs-android-engine)
#   target_link_libraries(your_lib demucs_android::engine)
#
# Options:
#   SDENGINE_JNI=ON      also build libsdengine_jni.so, the JNI binding for
#                        android/kotlin/NativeEngine.kt
#   SDENGINE_TOOLS=ON    desktop command-line tools and tests (default when
#                        this is the top-level project and not Android)
cmake_minimum_required(VERSION 3.22.1)
project(demucs_android_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(_tools_default OFF)
if(PROJECT_IS_TOP_LEVEL AND NOT ANDROID)
  set(_tools_default ON)
endif()
option(SDENGINE_TOOLS "Build the desktop tools and tests" ${_tools_default})
option(SDENGINE_JNI "Build the Android JNI binding (libsdengine_jni.so)" OFF)
set(SDENGINE_JNI_CLASS "io/github/jpaauwe/demucsengine/NativeEngine" CACHE STRING
    "The Kotlin/Java class the JNI binding registers its methods on")

set(SDENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(EIGEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/eigen" CACHE PATH "Eigen source tree")
if(NOT EXISTS "${EIGEN_DIR}/Eigen/Dense")
  message(FATAL_ERROR "Eigen not found at ${EIGEN_DIR}. Run: git submodule update --init")
endif()
set(VULKAN_HEADERS "" CACHE PATH "Vulkan headers for desktop builds, if not installed system-wide")

function(sdengine_library name)
  cmake_parse_arguments(ARG "PROFILE" "" "" ${ARGN})
  file(GLOB srcs "${SDENGINE_DIR}/demucs/*.cpp" "${SDENGINE_DIR}/compute/*.cpp")
  add_library(${name} STATIC ${srcs}
    "${SDENGINE_DIR}/engine.cpp" "${SDENGINE_DIR}/safetensors.cpp" "${SDENGINE_DIR}/sdprof.cpp")
  target_include_directories(${name} PUBLIC
    "${SDENGINE_DIR}" "${SDENGINE_DIR}/demucs" "${SDENGINE_DIR}/compute" "${EIGEN_DIR}")
  if(VULKAN_HEADERS)
    target_include_directories(${name} PRIVATE "${VULKAN_HEADERS}")
  endif()
  if(ANDROID)
    target_link_libraries(${name} PUBLIC dl)
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(${name} PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  endif()
  target_compile_options(${name} PRIVATE
    -O3 -fno-unsafe-math-optimizations -freciprocal-math -fno-signed-zeros
    -fexceptions -frtti -fvisibility=hidden -fvisibility-inlines-hidden
    -Wno-unused-parameter -Wno-sign-compare)
  target_compile_definitions(${name} PUBLIC NDEBUG EIGEN_NO_DEBUG)
  if(ARG_PROFILE)
    target_compile_definitions(${name} PUBLIC SD_PROFILE)
  endif()
endfunction()

sdengine_library(sdengine)
add_library(demucs_android::engine ALIAS sdengine)

if(SDENGINE_JNI)
  find_library(log-lib log)
  add_library(sdengine_jni SHARED android/jni/sdengine_jni.cpp)
  target_compile_definitions(sdengine_jni PRIVATE SDENGINE_JNI_CLASS="${SDENGINE_JNI_CLASS}")
  target_compile_options(sdengine_jni PRIVATE -O2 -fexceptions -frtti)
  target_link_libraries(sdengine_jni sdengine ${log-lib})
  # 16 KB page alignment, required for apps targeting Android 15+ devices.
  target_link_options(sdengine_jni PRIVATE "-Wl,-z,max-page-size=16384")
endif()

if(SDENGINE_TOOLS)
  find_package(Threads REQUIRED)
  add_executable(sdengine_cli tools/engine_cli.cpp)
  target_link_libraries(sdengine_cli sdengine Threads::Threads)

  add_executable(sdgpu_test tools/gpu_test.cpp)
  target_link_libraries(sdgpu_test sdengine Threads::Threads)

  sdengine_library(sdengine_prof PROFILE)
  add_executable(sdbench_cli tools/bench_cli.cpp bench/bench_core.cpp)
  target_include_directories(sdbench_cli PRIVATE bench)
  target_link_libraries(sdbench_cli sdengine_prof Threads::Threads)
endif()
