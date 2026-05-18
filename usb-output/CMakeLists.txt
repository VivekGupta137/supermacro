# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# sdkconfig.defaults does not override an existing sdkconfig; patch LWIP for HTTP UI.
set(_sdkconfig "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig")
if(EXISTS "${_sdkconfig}")
  file(READ "${_sdkconfig}" _sdkcontent)
  foreach(_pair
      "CONFIG_LWIP_MAX_SOCKETS=20"
      "CONFIG_LWIP_MAX_ACTIVE_TCP=16"
      "CONFIG_LWIP_PBUF_POOL_SIZE=24")
    string(REGEX REPLACE "([^=]+)=.*" "\\1" _key "${_pair}")
    string(REGEX REPLACE "[^=]+=(.*)" "\\1" _val "${_pair}")
    if(_sdkcontent MATCHES "${_key}=")
      string(REGEX REPLACE "${_key}=[^\n\r]*" "${_key}=${_val}" _sdkcontent "${_sdkcontent}")
    else()
      string(APPEND _sdkcontent "\n${_key}=${_val}\n")
    endif()
  endforeach()
  file(WRITE "${_sdkconfig}" "${_sdkcontent}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# MINIMAL_BUILD was enabled here before; it breaks CONFIG_MACRO_WEB_UI in main/CMakeLists and
# drops esp_http_server/json from the app graph while sdkconfig.h still enables the web UI code.
project(usb-keyboard-output)
