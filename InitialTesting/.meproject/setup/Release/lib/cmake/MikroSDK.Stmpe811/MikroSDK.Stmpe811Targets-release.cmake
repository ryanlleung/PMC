#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "MikroSDK.Stmpe811" for configuration "Release"
set_property(TARGET MikroSDK.Stmpe811 APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(MikroSDK.Stmpe811 PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/lib_stmpe811.a"
  )

list(APPEND _cmake_import_check_targets MikroSDK.Stmpe811 )
list(APPEND _cmake_import_check_files_for_MikroSDK.Stmpe811 "${_IMPORT_PREFIX}/lib/lib_stmpe811.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
