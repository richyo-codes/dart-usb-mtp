cmake_minimum_required(VERSION 3.13)
project(usb_sync_plugin LANGUAGES CXX)

set(PLUGIN_NAME "usb_sync_plugin")

add_library(${PLUGIN_NAME} SHARED
  "usb_sync_plugin.cc"
)

apply_standard_settings(${PLUGIN_NAME})

set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)

set(usb_sync_bundled_libraries
  ""
  PARENT_SCOPE
)
