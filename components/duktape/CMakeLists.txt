# Duktape 2.7.0 (https://duktape.org), the dist sources from duktape-2.7.0.tar.xz.
# Patched (duktape.patch, see README.md): exec timeout and native stack check hooks provided by
# main/script.c, fastint.
idf_component_register(SRCS "src/duktape.c"
                       INCLUDE_DIRS "src")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-but-set-variable -Wno-unused-variable
                       -Wno-unused-function -Wno-maybe-uninitialized)
