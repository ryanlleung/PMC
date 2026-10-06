#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "MikroSDK.Ssd1963" for configuration "Debug"
set_property(TARGET MikroSDK.Ssd1963 APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(MikroSDK.Ssd1963 PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/lib_ssd1963.a"
  )

list(APPEND _cmake_import_check_targets MikroSDK.Ssd1963 )
list(APPEND _cmake_import_check_files_for_MikroSDK.Ssd1963 "${_IMPORT_PREFIX}/lib/lib_ssd1963.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
