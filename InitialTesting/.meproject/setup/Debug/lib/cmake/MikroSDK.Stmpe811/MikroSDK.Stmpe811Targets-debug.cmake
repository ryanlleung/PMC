#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "MikroSDK.Stmpe811" for configuration "Debug"
set_property(TARGET MikroSDK.Stmpe811 APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(MikroSDK.Stmpe811 PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/lib_stmpe811.a"
  )

list(APPEND _cmake_import_check_targets MikroSDK.Stmpe811 )
list(APPEND _cmake_import_check_files_for_MikroSDK.Stmpe811 "${_IMPORT_PREFIX}/lib/lib_stmpe811.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
