# =============================================================================
# Resilient IoT Edge — Main Component Configuration
# =============================================================================

idf_component_register(
    SRCS
        "main.c"
        "storage_driver.c"       # [DEV1] LittleFS Write-First driver
    INCLUDE_DIRS
        "."
)