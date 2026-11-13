# Décodeur Helix AAC (RealNetworks, RPSL/RCSL — voir LICENSE/RPSL.txt), version
# d'ESP8266Audio (earlephilhower) ; voir NOTICE.md.
file(GLOB SRCS "src/*.c")
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "compat"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-maybe-uninitialized -Wno-misleading-indentation -Wno-stringop-overflow -Wno-array-bounds -Wno-implicit-fallthrough -Wno-unused-parameter)
