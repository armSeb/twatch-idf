# Pilote BMA423 de Bosch Sensortec (BSD-3-Clause), avec le microcode des
# fonctions embarquées (compteur de pas, lever de poignet)
idf_component_register(SRCS "src/bma4.c" "src/bma423.c"
                       INCLUDE_DIRS "include")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-function -Wno-maybe-uninitialized)
