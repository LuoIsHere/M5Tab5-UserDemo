set(TAB5X_MAIN_SRCS "app_main.cpp")
set(TAB5X_MAIN_INCS ".")

# Keep the boot MP3 diagnostic independent from the normal application/HAL source
# tree so the isolated image does not create UI or peripheral-service tasks.
if(CONFIG_TAB5X_BOOT_MODE_MP3_ISOLATED)
    list(APPEND TAB5X_MAIN_SRCS
        "diagnostics/boot_mp3/boot_mp3_test.cpp"
    )
else()
    file(GLOB_RECURSE APP_LAYER_SRCS
        ../../../app/*.c
        ../../../app/*.cc
        ../../../app/*.cpp
    )

    file(GLOB_RECURSE MY_HAL_SRCS
        ./hal/*.c
        ./hal/*.cc
        ./hal/*.cpp
    )

    list(APPEND TAB5X_MAIN_SRCS ${APP_LAYER_SRCS} ${MY_HAL_SRCS})
    list(APPEND TAB5X_MAIN_INCS ../../../app)
endif()

idf_component_register(
    SRCS ${TAB5X_MAIN_SRCS}
    INCLUDE_DIRS ${TAB5X_MAIN_INCS}
    EMBED_TXTFILES
        "../audio/canon_in_d.mp3"
        "../audio/startup_sfx.mp3"
        "../audio/shutdown_sfx.mp3"
)
