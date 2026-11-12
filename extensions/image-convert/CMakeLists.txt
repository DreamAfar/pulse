cmake_minimum_required(VERSION 3.25)
# Static CRT and static dependencies: the package is a single DLL.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
project(PulseImageConvert LANGUAGES C CXX)

set(PULSE_SDK_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../sdk/native/include" CACHE PATH "Public Pulse SDK headers")

find_package(WebP CONFIG REQUIRED)
find_package(libheif CONFIG REQUIRED)
find_package(Stb REQUIRED)

# libjxl and the codecs behind libheif ship no CMake package in vcpkg.
set(IMAGE_CONVERT_LIBS)
foreach(spec "jxl" "jxl_threads" "jxl_cms" "hwy" "brotlienc;brotlienc-static" "brotlidec;brotlidec-static"
             "brotlicommon;brotlicommon-static" "lcms2" "de265;libde265" "dav1d;libdav1d")
    list(GET spec 0 first)
    find_library(IMAGE_CONVERT_${first} NAMES ${spec} REQUIRED)
    list(APPEND IMAGE_CONVERT_LIBS "${IMAGE_CONVERT_${first}}")
endforeach()

add_library(image_convert_core STATIC
    src/decode.cpp src/encode.cpp src/exif.cpp src/wic.cpp src/convert_job.cpp)
target_include_directories(image_convert_core PUBLIC "${PULSE_SDK_INCLUDE}" "${Stb_INCLUDE_DIR}")
target_link_libraries(image_convert_core PUBLIC
    WebP::webp WebP::webpdemux heif ${IMAGE_CONVERT_LIBS} windowscodecs ole32 oleaut32 uuid)
target_compile_definitions(image_convert_core PUBLIC UNICODE _UNICODE NOMINMAX)

foreach(target image_convert_core)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_options(${target} PUBLIC /W4 /permissive- /utf-8 /EHsc)
endforeach()

add_library(image_convert SHARED src/plugin.cpp)
target_link_libraries(image_convert PRIVATE image_convert_core)

option(IMAGE_CONVERT_TESTS "Build the converter test" ON)
if(IMAGE_CONVERT_TESTS)
    add_executable(image_convert_test test/converter_test.cpp)
    target_link_libraries(image_convert_test PRIVATE image_convert_core)
endif()
