#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "MikroSDK.Ethernet.CycloneTCP.Config" for configuration "Release"
set_property(TARGET MikroSDK.Ethernet.CycloneTCP.Config APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(MikroSDK.Ethernet.CycloneTCP.Config PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/lib_cyclone_tcp_config.a"
  )

list(APPEND _cmake_import_check_targets MikroSDK.Ethernet.CycloneTCP.Config )
list(APPEND _cmake_import_check_files_for_MikroSDK.Ethernet.CycloneTCP.Config "${_IMPORT_PREFIX}/lib/lib_cyclone_tcp_config.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
