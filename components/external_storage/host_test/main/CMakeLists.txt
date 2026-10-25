# Host test compiles ONLY the pure parts: the detect debouncer and the
# probe of what a card holds. No GPIO/SDMMC.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/es_detect.c"
         "${comp_dir}/es_probe.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
