# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS 
    "../../dependencies"
) 

project(m5stack_tab5)

if(IDF_VERSION_MAJOR GREATER_EQUAL 6 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    idf_component_get_property(lvgl_lib lvgl COMPONENT_LIB)
    target_compile_options(${lvgl_lib} PRIVATE "-Wno-error=attributes")

    idf_component_get_property(smooth_ui_toolkit_lib smooth_ui_toolkit COMPONENT_LIB)
    target_compile_options(${smooth_ui_toolkit_lib} PRIVATE "-Wno-error=deprecated")
endif()
