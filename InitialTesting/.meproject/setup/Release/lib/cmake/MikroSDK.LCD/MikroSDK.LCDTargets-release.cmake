#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "MikroSDK.LCD" for configuration "Release"
set_property(TARGET MikroSDK.LCD APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(MikroSDK.LCD PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/lib_lcd.a"
  )

list(APPEND _cmake_import_check_targets MikroSDK.LCD )
list(APPEND _cmake_import_check_files_for_MikroSDK.LCD "${_IMPORT_PREFIX}/lib/lib_lcd.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
