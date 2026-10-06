#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "MikroSDK.Driver.CAN" for configuration "Debug"
set_property(TARGET MikroSDK.Driver.CAN APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(MikroSDK.Driver.CAN PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/lib_drv_can.a"
  )

list(APPEND _cmake_import_check_targets MikroSDK.Driver.CAN )
list(APPEND _cmake_import_check_files_for_MikroSDK.Driver.CAN "${_IMPORT_PREFIX}/lib/lib_drv_can.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
