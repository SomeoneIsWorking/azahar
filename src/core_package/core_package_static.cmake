# The static core package: libazahar_core.a holds the members of every static library the core
# libraries link, so a frontend links one archive and the system libraries the config names.

# Appends to the global property AZAHAR_CORE_ARCHIVES each static library `target` reaches
# through its link libraries, and to AZAHAR_CORE_SYSTEM each system library: one named by file
# name or flag, or an imported one.
function(azahar_core_walk target)
    get_target_property(aliased ${target} ALIASED_TARGET)
    if (aliased)
        set(target ${aliased})
    endif()
    get_property(seen GLOBAL PROPERTY AZAHAR_CORE_SEEN)
    if (target IN_LIST seen)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY AZAHAR_CORE_SEEN ${target})
    get_target_property(type ${target} TYPE)
    get_target_property(imported ${target} IMPORTED)
    if (type STREQUAL "STATIC_LIBRARY" AND NOT imported)
        set_property(GLOBAL APPEND PROPERTY AZAHAR_CORE_ARCHIVES "$<TARGET_FILE:${target}>")
    elseif (imported AND NOT type STREQUAL "INTERFACE_LIBRARY")
        # A library found on the system stays the system's: the frontend links it by its file.
        set_property(GLOBAL APPEND PROPERTY AZAHAR_CORE_SYSTEM "$<TARGET_LINKER_FILE:${target}>")
    endif()
    set(links "")
    if (NOT imported AND NOT type STREQUAL "INTERFACE_LIBRARY")
        get_target_property(own ${target} LINK_LIBRARIES)
        if (own)
            list(APPEND links ${own})
        endif()
    endif()
    get_target_property(interface ${target} INTERFACE_LINK_LIBRARIES)
    if (interface)
        list(APPEND links ${interface})
    endif()
    foreach (link IN LISTS links)
        # $<LINK_ONLY:x> is how a static library records a private dependency.
        string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" link "${link}")
        if (link MATCHES "^\\$<" OR link STREQUAL "")
            continue()
        elseif (TARGET ${link})
            azahar_core_walk(${link})
        else()
            set_property(GLOBAL APPEND PROPERTY AZAHAR_CORE_SYSTEM ${link})
        endif()
    endforeach()
endfunction()

# Runs once every target is defined: merges the archives and writes the package's config.
function(azahar_core_merge libraries definitions package_dir)
    foreach (library IN LISTS libraries)
        azahar_core_walk(${library})
    endforeach()
    get_property(archives GLOBAL PROPERTY AZAHAR_CORE_ARCHIVES)
    get_property(system GLOBAL PROPERTY AZAHAR_CORE_SYSTEM)
    list(REMOVE_DUPLICATES system)
    set(merged "${CMAKE_CURRENT_BINARY_DIR}/libazahar_core.a")
    set(script "CREATE ${merged}\n")
    foreach (archive IN LISTS archives)
        string(APPEND script "ADDLIB ${archive}\n")
    endforeach()
    string(APPEND script "SAVE\nEND\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/merge.mri" CONTENT "${script}")
    add_custom_command(OUTPUT "${merged}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${merged}"
        COMMAND "${CMAKE_AR}" -M < "${CMAKE_CURRENT_BINARY_DIR}/merge.mri"
        DEPENDS ${libraries} "${CMAKE_CURRENT_BINARY_DIR}/merge.mri"
        COMMENT "Merging the core's static libraries into libazahar_core.a")
    add_custom_target(azahar_core ALL DEPENDS "${merged}")
    install(FILES "${merged}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    set(config "add_library(AzaharCore::azahar_core STATIC IMPORTED)\n")
    string(APPEND config "get_filename_component(_azahar_prefix \"\${CMAKE_CURRENT_LIST_DIR}/../../..\" ABSOLUTE)\n")
    string(APPEND config "set_target_properties(AzaharCore::azahar_core PROPERTIES\n")
    string(APPEND config "  IMPORTED_LOCATION \"\${_azahar_prefix}/${CMAKE_INSTALL_LIBDIR}/libazahar_core.a\"\n")
    string(APPEND config "  INTERFACE_INCLUDE_DIRECTORIES \"\${_azahar_prefix}/${CMAKE_INSTALL_INCLUDEDIR}/azahar\"\n")
    string(APPEND config "  INTERFACE_COMPILE_DEFINITIONS \"${definitions}\"\n")
    string(APPEND config "  INTERFACE_COMPILE_FEATURES cxx_std_20\n")
    string(APPEND config "  INTERFACE_LINK_LIBRARIES \"${system}\")\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/AzaharCoreConfig.cmake" CONTENT "${config}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/AzaharCoreConfig.cmake" DESTINATION "${package_dir}")
endfunction()

function(azahar_core_static_archive libraries definitions package_dir)
    # The core's libraries gain links after this directory is read; the walk waits for all of them.
    cmake_language(EVAL CODE "
        cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL
            azahar_core_merge [[${libraries}]] [[${definitions}]] [[${package_dir}]])")
endfunction()
