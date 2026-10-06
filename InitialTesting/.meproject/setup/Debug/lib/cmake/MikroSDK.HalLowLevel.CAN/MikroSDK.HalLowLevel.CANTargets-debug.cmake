#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "MikroSDK.HalLowLevel.CAN" for configuration "Debug"
set_property(TARGET MikroSDK.HalLowLevel.CAN APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(MikroSDK.HalLowLevel.CAN PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/lib_hal_ll_can.a"
  )

list(APPEND _cmake_import_check_targets MikroSDK.HalLowLevel.CAN )
list(APPEND _cmake_import_check_files_for_MikroSDK.HalLowLevel.CAN "${_IMPORT_PREFIX}/lib/lib_hal_ll_can.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
