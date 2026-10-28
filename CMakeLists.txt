cmake_minimum_required(VERSION 3.16)

# Permet un build hors-ligne (LVGL local) : TW_EXTRA_COMPONENT_DIRS=/chemin
if(DEFINED ENV{TW_EXTRA_COMPONENT_DIRS})
    set(EXTRA_COMPONENT_DIRS $ENV{TW_EXTRA_COMPONENT_DIRS})
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Build de diagnostic « BLE seul » : TW_DIAG_BLE_ONLY=1 idf.py build
if(DEFINED ENV{TW_DIAG_BLE_ONLY})
    idf_build_set_property(COMPILE_DEFINITIONS "TW_DIAG_BLE_ONLY=1" APPEND)
endif()

if(DEFINED ENV{TW_DIAG_BARE})
    idf_build_set_property(COMPILE_DEFINITIONS "TW_DIAG_BARE=1" APPEND)
endif()

# Version affichée (À propos, page de mise à jour) et comparée lors d'une OTA
set(PROJECT_VER "0.9.0")

project(twatch)
