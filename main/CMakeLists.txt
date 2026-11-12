file(GLOB_RECURSE app_sources ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

# Firmware sans l'app « TV off » ni sa base de codes CC BY-SA 4.0 :
#   TW_NO_TVOFF=1 idf.py build
if(DEFINED ENV{TW_NO_TVOFF})
    list(REMOVE_ITEM app_sources ${CMAKE_CURRENT_SOURCE_DIR}/app_tvoff.c)
endif()

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
)
if(DEFINED ENV{TW_NO_TVOFF})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TW_NO_TVOFF=1)
endif()

# Avertissements stricts sur notre code (pas sur les polices générées)
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ancs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ble.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ui.c
    ${CMAKE_CURRENT_SOURCE_DIR}/power.c
    ${CMAKE_CURRENT_SOURCE_DIR}/notif_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ancs_parse.c
    ${CMAKE_CURRENT_SOURCE_DIR}/face_classic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/face_pub.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_chrono.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_timer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_lamp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/i18n.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_tvoff.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_rfgen.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_wifi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_sdr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sdr_rx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sdr_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_webrx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/webrx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/webrx_cfg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sound.c
    ${CMAKE_CURRENT_SOURCE_DIR}/crashlog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gauge.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ota.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter;-Wno-missing-field-initializers"
)
