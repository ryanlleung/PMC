cmake_minimum_required(VERSION 3.19)

if (${TOOLCHAIN_LANGUAGE} MATCHES "MikroC")
    project(InitialTesting LANGUAGES MikroC)
else()
    project(InitialTesting LANGUAGES C ASM)
endif()

include(mikroeUtils)

# Build Debug. A Release build blanks the display: MikroE's prebuilt Release
# mikroSDK libraries (lib_ssd1963.a) were compiled with a non-volatile GPIO
# register struct, so ssd1963_write_command loses its WR low and the SSD1963
# never latches a command (found on the bench, 0.3.33; confirmed by
# disassembly, 0.8.9). LVGL, the pixel loop (driver/display_px.c) and the
# ui*.c screens are compiled
# at -O2 below while the SDK libraries stay Debug. driver/display_lvgl.c
# must stay at -O0: it expands the static inline write_command/frame_start
# from the SDK lvgl_common.h, which hit the same non-volatile GPIO struct
# (0.8.9 on the bench: nothing reached the panel). The hardware drivers stay
# at -O0 too: some bit-bang or count delay loops (clicks.c, extflash.c,
# eth_hw.c), so their timing does not change.
include(${CMAKE_CURRENT_SOURCE_DIR}/1ms_timer/TimerHeaderConfig.cmake)
set(LVGL_VERSION "9.4.0" CACHE STRING "" FORCE)
set(PROJECT_TYPE "lvgl_designer" CACHE STRING "" FORCE)

add_executable(InitialTesting
    driver/display_lvgl.h
    driver/lv_port_indev.h
    driver/display_lvgl.c
    driver/display_px.h
    driver/display_px.c
    driver/lv_port_indev.c
    1ms_Timer.h
    resource.mres
    main_screen.mscr
    main_screen.c
    main_screen.h
    motor_screen.c
    motor_screen.h
    limit.c
    limit.h
    usb_serial.h
    usb_serial.c
    tinyusb_cdc_device.c
    clicks.h
    clicks.c
    baro.h
    baro.c
    stepper3.h
    stepper3.c
    valve.h
    valve.c
    valvepos.h
    valvepos.c
    trend.h
    trend.c
    ctrl.h
    ctrl.c
    tune.c
    sense.h
    sense.c
    remote.h
    remote.c
    extflash.h
    extflash.c
    cal.h
    cal.c
    cfg.h
    cfg.c
    ui.h
    ui.c
    ui_home.c
    ui_valve.c
    ui_settings.c
    ui_trend.c
    ui_font_12.c
    ui_font_16.c
    ui_font_26.c
    ui_font_40.c
    ui_img_logo.c
    sysinfo.h
    sysinfo.c
    rtclock.c
    fixed.h
    fixed.c
    pmc_config.h
    link.h
    link.c
    eth_hw.h
    eth_hw.c
    ethlink.h
    ethlink.c
    modbus.h
    modbus.c
    mbmap.c
    main.c
)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS resource.mres)
############################ InitialTesting GENERATED CODE START ###########################
############################ InitialTesting GENERATED CODE END ###########################

find_package(MikroC.Core REQUIRED)
find_package_me(MikroSDK.LVGL.Common REQUIRED)
find_package_me(MikroSDK.GraphicLibrary REQUIRED)
find_package_me(MikroSDK.USB.Tiny REQUIRED)
find_package_me(MikroSDK.Ethernet.CycloneTCP REQUIRED)

lvgl_designer_generator(InitialTesting resource.mres main_screen.mscr)

target_link_libraries(InitialTesting
    PUBLIC
        MikroC.Core
        MikroSDK.LVGL.Common
        MikroSDK.GraphicLibrary
        MikroSDK.USB.Tiny
        MikroSDK.Ethernet.CycloneTCP
        lvgl
)

target_compile_definitions(InitialTesting PUBLIC
    ${tim_list}
)

target_include_directories(InitialTesting
    PUBLIC
        driver
        1ms_timer
        .
)

if((${MCU_FLASH} LESS 184320) OR (${MCU_RAM} LESS 24576)) ## 180KB Flash / 24KB RAM recommended.
    message(FATAL_ERROR ": ${MCU_NAME} does not have recommended memory for this example.")
endif()

if(COMPILER_REVISION)
    if(${COMPILER_REVISION} VERSION_GREATER_EQUAL "1.0")
        if(${TOOLCHAIN_LANGUAGE} STREQUAL "GNU")
            if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.meproject)
                fetch_startup_name(${CMAKE_CURRENT_SOURCE_DIR}/.meproject startupName)
                if(startupName)
                    add_startup_all_targets(. ${CMAKE_CURRENT_SOURCE_DIR} ${startupName})
                endif()
                fetch_linker_name(${CMAKE_CURRENT_SOURCE_DIR}/.meproject linkerName)
                if(linkerName)
                    add_ld_all_targets(. ${CMAKE_CURRENT_SOURCE_DIR} ${linkerName})
                endif()
            endif()
        endif()
    endif()
endif()
add_subdirectory(lvgl_940)

set_source_files_properties(driver/display_px.c ui.c ui_home.c ui_valve.c ui_settings.c ui_trend.c
    PROPERTIES COMPILE_OPTIONS "-O2")
get_target_property(_lvgl_real lvgl ALIASED_TARGET)
if(NOT _lvgl_real)
    set(_lvgl_real lvgl)
endif()
target_compile_options(${_lvgl_real} PRIVATE -O2)
