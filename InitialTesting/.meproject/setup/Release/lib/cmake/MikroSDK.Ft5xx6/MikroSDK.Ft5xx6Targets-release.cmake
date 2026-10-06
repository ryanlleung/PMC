#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "MikroSDK.Ft5xx6" for configuration "Release"
set_property(TARGET MikroSDK.Ft5xx6 APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(MikroSDK.Ft5xx6 PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/lib_ft5xx6.a"
  )

list(APPEND _cmake_import_check_targets MikroSDK.Ft5xx6 )
list(APPEND _cmake_import_check_files_for_MikroSDK.Ft5xx6 "${_IMPORT_PREFIX}/lib/lib_ft5xx6.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
