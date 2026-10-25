idf_component_register(
    SRCS "external_storage.c"
         "external_storage_settings.c"
         "es_detect.c"
         "es_probe.c"
         "external_storage_cli.c"
         "external_storage_http.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # HAL: owns the SD device + its FAT mount at /sd. File ACCESS goes
    # through the filesystem component (main glues the mount events to it:
    # no edge between the two here, in either direction). cmdline_manager
    # only by the _cli.c (`sdcard` command), http_server_manager only by
    # the _http.c (/api/sdcard), esp_timer for the format's timing.
    PRIV_REQUIRES esp_driver_sdmmc fatfs esp_driver_gpio esp_timer
                  dev_status_manager log_manager cmdline_manager
                  http_server_manager settings_manager
)
