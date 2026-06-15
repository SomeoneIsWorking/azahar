# SoH3D oracle: a standalone tool that decodes PICA textures with Azahar's own
# decoder, to diff against the SoH3D converter. Links only citra_common + the two
# texture-decode translation units (no GL/Vulkan/core), so it builds cheaply.
add_executable(soh3d_oracle
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../video_core/texture/texture_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../video_core/texture/etc1.cpp
)
target_include_directories(soh3d_oracle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(soh3d_oracle PRIVATE citra_common)
