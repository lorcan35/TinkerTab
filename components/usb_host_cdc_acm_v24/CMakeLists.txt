idf_component_register(
    SRCS
        "cdc_acm_host.c"
        "cdc_host_descriptor_parsing.c"
        "cdc_host_acm_compliant.c"
        "cdc_host_ops.c"
    INCLUDE_DIRS "include" "interface"
    PRIV_INCLUDE_DIRS "private_include" "include/esp_private"
    REQUIRES usb
)
