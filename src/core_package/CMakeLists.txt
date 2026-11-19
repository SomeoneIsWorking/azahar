# The core as one installable library with its headers and a CMake package, so frontends outside
# this tree build against a prefix: find_package(AzaharCore CONFIG).
# Shared by default: configure with -DCMAKE_POSITION_INDEPENDENT_CODE=ON; the static core and
# externals link into it. Static on Windows, where a DLL holds at most 65535 exports (the core has
# over 90000) and the frontend links its C++ runtime statically: one archive of every static
# library the core links, merged by core_package_static.cmake.
option(AZAHAR_CORE_STATIC "Install the core as one static archive instead of a shared library" ${WIN32})

include(GNUInstallDirs)
set(package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/AzaharCore")
set(core_libraries citra_common citra_core video_core audio_core)
get_directory_property(core_definitions COMPILE_DEFINITIONS)

if (AZAHAR_CORE_STATIC)
    include("${CMAKE_CURRENT_SOURCE_DIR}/core_package_static.cmake")
    azahar_core_static_archive("${core_libraries}" "${core_definitions}" "${package_dir}")
else()
    add_library(azahar_core SHARED core_package.cpp)
    add_library(AzaharCore::azahar_core ALIAS azahar_core)
    string(REPLACE ";" "," whole "${core_libraries}")
    target_link_libraries(azahar_core PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${whole}>")
    if (APPLE)
        # common's Apple helpers (apple_utils.mm) call AppKit and CoreGraphics, which a frontend outside
        # this tree does not link.
        target_link_libraries(azahar_core PRIVATE "-framework AppKit" "-framework Foundation" "-framework CoreGraphics")
    endif()
    target_compile_definitions(azahar_core INTERFACE ${core_definitions})
    target_compile_features(azahar_core INTERFACE cxx_std_20)
    target_include_directories(azahar_core INTERFACE $<INSTALL_INTERFACE:include/azahar>)
    install(TARGETS azahar_core EXPORT AzaharCoreTargets LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(EXPORT AzaharCoreTargets NAMESPACE AzaharCore:: DESTINATION "${package_dir}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/AzaharCoreConfig.cmake"
        "include(\"\${CMAKE_CURRENT_LIST_DIR}/AzaharCoreTargets.cmake\")\n")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/AzaharCoreConfig.cmake" DESTINATION "${package_dir}")
endif()

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
