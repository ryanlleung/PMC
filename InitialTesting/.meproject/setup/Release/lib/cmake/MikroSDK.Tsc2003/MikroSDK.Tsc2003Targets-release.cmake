#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "MikroSDK.Tsc2003" for configuration "Release"
set_property(TARGET MikroSDK.Tsc2003 APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(MikroSDK.Tsc2003 PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/lib_tsc2003.a"
  )

list(APPEND _cmake_import_check_targets MikroSDK.Tsc2003 )
list(APPEND _cmake_import_check_files_for_MikroSDK.Tsc2003 "${_IMPORT_PREFIX}/lib/lib_tsc2003.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
