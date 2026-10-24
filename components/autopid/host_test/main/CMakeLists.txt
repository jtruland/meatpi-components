# Pure modules only: scheduler, response parser, config parse (+ the
# expression_parser dependency, itself pure). AUTOPID_HOST_TEST strips
# the target-only halves (file IO, cache).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")
set(ep_dir "${CMAKE_CURRENT_LIST_DIR}/../../../expression_parser")
# the chip driver's pure parse module carries the EEPROM guard that
# ap_init_sanitize() delegates to (obd_chip_guard.h, 2026-09-16)
set(oc_dir "${CMAKE_CURRENT_LIST_DIR}/../../../obd_chip")
# the J1939 rows decode the j1939 component's pure SPN table
set(j_dir "${CMAKE_CURRENT_LIST_DIR}/../../../j1939")

idf_component_register(
    SRCS "test_main.c"
         "test_names.c"
         "test_vehicle.c"
         "test_vehicle_index.c"
         "test_bus_guard.c"
         "test_dialect.c"
         "test_dtc_report.c"
         "test_resp_lines.c"
         "test_j1939_rows.c"
         "test_scan_rows.c"
         "test_chain.c"
         "test_bounds.c"
         "test_frames.c"
         "${comp_dir}/autopid_sched.c"
         "${comp_dir}/autopid_scan_rows.c"
         "${comp_dir}/autopid_bus_guard.c"
         "${comp_dir}/autopid_dialect.c"
         "${comp_dir}/autopid_chain.c"
         "${comp_dir}/autopid_bounds.c"
         "${comp_dir}/autopid_resp.c"
         "${comp_dir}/autopid_resp_lines.c"
         "${comp_dir}/autopid_resp_filter.c"
         "${comp_dir}/autopid_config.c"
         "${comp_dir}/autopid_names.c"
         "${comp_dir}/autopid_std.c"
         "${comp_dir}/autopid_vehicle_core.c"
         "${comp_dir}/autopid_vehicle_index.c"
         "${comp_dir}/autopid_vehicle_codec.c"
         "${comp_dir}/autopid_dtc_codec.c"
         "${comp_dir}/autopid_dtc_db_codec.c"
         "${comp_dir}/autopid_dbc_codec.c"
         "${comp_dir}/autopid_j1939_core.c"
         "${j_dir}/j1939_spn_core.c"
         "${ep_dir}/expression_parser.c"
         "${oc_dir}/obd_chip_parse.c"
    INCLUDE_DIRS "${comp_dir}" "${comp_dir}/include" "${ep_dir}/include"
                 "${oc_dir}/include" "${oc_dir}" "${j_dir}/include"
    REQUIRES unity
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE AUTOPID_HOST_TEST=1)
