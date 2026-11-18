#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "MikroSDK.Driver.RTC" for configuration "Release"
set_property(TARGET MikroSDK.Driver.RTC APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(MikroSDK.Driver.RTC PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/lib_drv_rtc.a"
  )

list(APPEND _cmake_import_check_targets MikroSDK.Driver.RTC )
list(APPEND _cmake_import_check_files_for_MikroSDK.Driver.RTC "${_IMPORT_PREFIX}/lib/lib_drv_rtc.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
