file(GLOB_RECURSE app_sources ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
)

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
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter;-Wno-missing-field-initializers"
)
