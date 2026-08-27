idf_component_register(
    SRCS "main.c" "acquisition.c"
    INCLUDE_DIRS "."
    REQUIRES sc16is752 h1 esp_timer esp_system
)
