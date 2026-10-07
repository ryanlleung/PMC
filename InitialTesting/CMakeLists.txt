cmake_minimum_required(VERSION 3.19)

if (${TOOLCHAIN_LANGUAGE} MATCHES "MikroC")
    project(InitialTesting LANGUAGES MikroC)
else()
    project(InitialTesting LANGUAGES C ASM)
endif()

include(mikroeUtils)
include(${CMAKE_CURRENT_SOURCE_DIR}/1ms_timer/TimerHeaderConfig.cmake)
set(LVGL_VERSION "9.4.0" CACHE STRING "" FORCE)
set(PROJECT_TYPE "lvgl_designer" CACHE STRING "" FORCE)

add_executable(InitialTesting
    driver/display_lvgl.h
    driver/lv_port_indev.h
    driver/display_lvgl.c
    driver/lv_port_indev.c
    1ms_Timer.h
    resource.mres
    main_screen.mscr
    main_screen.c
    main_screen.h
    usb_serial.h
    usb_serial.c
    clicks.h
    clicks.c
    extflash.h
    extflash.c
    cal.h
    cal.c
    sysinfo.h
    sysinfo.c
    pmc_config.h
    link.h
    link.c
    eth_hw.h
    eth_hw.c
    ethlink.h
    ethlink.c
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
