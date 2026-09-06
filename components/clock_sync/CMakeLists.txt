idf_component_register(SRCS "clock_sync.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ab_protocol esp_timer freertos)
