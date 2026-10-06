#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "MikroSDK.VisualTft" for configuration "Debug"
set_property(TARGET MikroSDK.VisualTft APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(MikroSDK.VisualTft PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/lib_vtft.a"
  )

list(APPEND _cmake_import_check_targets MikroSDK.VisualTft )
list(APPEND _cmake_import_check_files_for_MikroSDK.VisualTft "${_IMPORT_PREFIX}/lib/lib_vtft.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
