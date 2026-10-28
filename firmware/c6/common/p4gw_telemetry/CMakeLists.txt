# WHOLE_ARCHIVE: the RCP hook below overrides a weak default inside the openthread component.
idf_component_register(SRCS "p4gw_telemetry.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_tsens esp_app_format esp_timer
                       WHOLE_ARCHIVE)
