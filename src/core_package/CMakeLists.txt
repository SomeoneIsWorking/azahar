# The core as one installable shared library with its headers and a CMake package, so frontends
# outside this tree build against a prefix: find_package(AzaharCore CONFIG).
# Configure with -DCMAKE_POSITION_INDEPENDENT_CODE=ON; the static core and externals link into it.
add_library(azahar_core SHARED core_package.cpp)
add_library(AzaharCore::azahar_core ALIAS azahar_core)
target_link_libraries(azahar_core PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,citra_common,citra_core,video_core,audio_core>")

get_directory_property(core_definitions COMPILE_DEFINITIONS)
target_compile_definitions(azahar_core INTERFACE ${core_definitions})
target_compile_features(azahar_core INTERFACE cxx_std_20)
target_include_directories(azahar_core INTERFACE $<INSTALL_INTERFACE:include/azahar>)

include(GNUInstallDirs)
set(header_patterns FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.inc" PATTERN "*.ipp")
set(include_dir "${CMAKE_INSTALL_INCLUDEDIR}/azahar")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/" DESTINATION "${include_dir}" ${header_patterns})
install(DIRECTORY "${PROJECT_BINARY_DIR}/src/common/" DESTINATION "${include_dir}/common" ${header_patterns}
    PATTERN "CMakeFiles" EXCLUDE)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/externals/fmt/include/" DESTINATION "${include_dir}" ${header_patterns})
install(DIRECTORY "${PROJECT_SOURCE_DIR}/externals/boost/boost" DESTINATION "${include_dir}" ${header_patterns})
install(FILES "${PROJECT_SOURCE_DIR}/externals/microprofile/microprofile.h"
    "${PROJECT_SOURCE_DIR}/externals/microprofile/microprofileui.h"
    "${PROJECT_SOURCE_DIR}/externals/dds-ktx/dds-ktx.h"
    "${PROJECT_SOURCE_DIR}/externals/xxHash/xxhash.h"
    DESTINATION "${include_dir}")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/externals/xbyak/xbyak" DESTINATION "${include_dir}" ${header_patterns})

install(TARGETS azahar_core EXPORT AzaharCoreTargets LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
set(package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/AzaharCore")
install(EXPORT AzaharCoreTargets NAMESPACE AzaharCore:: DESTINATION "${package_dir}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/AzaharCoreConfig.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/AzaharCoreTargets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/AzaharCoreConfig.cmake" DESTINATION "${package_dir}")
