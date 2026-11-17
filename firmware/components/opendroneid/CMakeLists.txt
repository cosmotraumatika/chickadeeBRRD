idf_component_register(
    SRCS "opendroneid.c"
    INCLUDE_DIRS "."
)
# Third-party code, built as shipped: do not let its warnings fail our build.
target_compile_options(${COMPONENT_LIB} PRIVATE -w -DODID_DISABLE_PRINTF)
