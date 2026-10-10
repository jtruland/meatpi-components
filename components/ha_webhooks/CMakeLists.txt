idf_component_register(
    SRCS "ha_webhooks.c"
         "ha_webhooks_settings.c"
         "ha_webhooks_http.c"
         "ha_webhooks_post.c"
         "ha_webhooks_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # settings/log descriptors; the /api/webhook route; outbound POST;
    # the payload sources (autopid snapshot + cached config, dev_status,
    # battery voltage, vpn tunnel ip, mdns hostname, the GPS fix cache);
    # the console command
    PRIV_REQUIRES settings_manager log_manager http_server_manager
                  http_client_manager autopid dev_status_manager
                  battery_monitor vpn_manager mdns_manager obd_chip
                  cmdline_manager usb_acm_cli wifi_manager
)
