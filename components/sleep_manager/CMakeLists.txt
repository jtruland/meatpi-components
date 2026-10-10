idf_component_register(
    SRCS "sleep_manager.c"
         "sleep_manager_settings.c"
         "sleep_manager_policy.c"
         "sleep_manager_events.c"
         "sleep_manager_http.c"
         "sleep_manager_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: the low-power ladder. Voltage comes from battery_monitor
    # (no second ADC owner); the ordered shutdown sequence is main's
    # prepare callback, so no dependency on the radio/feature managers.
    # /api/sleep and the `sleep` command are optional (main wires them).
    PRIV_REQUIRES battery_monitor obd_chip dev_status_manager
                  restart_tracker event_manager http_server_manager
                  settings_manager log_manager cmdline_manager
                  esp_driver_gpio esp_hw_support imu_manager
)
