cmake_minimum_required(VERSION 3.21)

# WebP/AVIF encoders come from vcpkg (manifest mode, see vcpkg.json). Without them
# the app still builds; those two formats are then hidden from the wheel.
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
  set(VCPKG_TARGET_TRIPLET x64-windows-static CACHE STRING "")
endif()

project(FileTools VERSION 1.3.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(FileTools WIN32
  src/main.cpp
  src/common.cpp
  src/gfx.cpp
  src/image.cpp
  src/pdf.cpp
  src/ocr.cpp
  src/zip.cpp
  src/docs.cpp
  src/media.cpp
  src/shellutil.cpp
  src/actions.cpp
  src/wheel.cpp
  src/floatshelf.cpp
  src/editor.cpp
  src/bgremove.cpp
  src/vectorize.cpp
  src/svg.cpp
  src/svgtext.cpp
  src/capture.cpp
  src/launcher.cpp
  src/fileinfo.cpp
  src/infopanel.cpp
  src/glass.cpp
  src/qrcode.cpp
  src/fileserver.cpp
  src/sendpanel.cpp
  src/settingspanel.cpp
  res/FileTools.manifest
  res/FileTools.rc
)

# WINRT_NO_SOURCE_LOCATION keeps C++/WinRT from embedding absolute source paths in the exe.
target_compile_definitions(FileTools PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINRT_NO_SOURCE_LOCATION
  "FT_APP_VERSION=\"${PROJECT_VERSION}\"")
target_compile_options(FileTools PRIVATE /utf-8 /W3 /permissive- /bigobj /EHsc /MP)
target_link_libraries(FileTools PRIVATE
  windowscodecs gdiplus shlwapi shcore ole32 oleaut32 uuid propsys windowsapp comctl32 bcrypt dwmapi uxtheme d2d1 dwrite ws2_32 iphlpapi xmllite)

find_package(WebP CONFIG QUIET)
if(WebP_FOUND)
  target_link_libraries(FileTools PRIVATE WebP::webp WebP::webpdecoder)
  target_compile_definitions(FileTools PRIVATE FT_HAVE_WEBP)
  message(STATUS "WebP: enabled")
endif()

find_package(libavif CONFIG QUIET)
if(libavif_FOUND)
  target_link_libraries(FileTools PRIVATE avif)
  target_compile_definitions(FileTools PRIVATE FT_HAVE_AVIF)
  message(STATUS "AVIF: enabled")
endif()

# Without lunasvg, SVG falls back to Direct2D, which lacks <pattern>, <mask>, <marker> and <style>.
find_package(lunasvg CONFIG QUIET)
if(lunasvg_FOUND)
  target_link_libraries(FileTools PRIVATE lunasvg::lunasvg)
  target_compile_definitions(FileTools PRIVATE FT_HAVE_LUNASVG)
  message(STATUS "lunasvg: enabled")
endif()
