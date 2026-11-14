// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#version 450 core
#extension GL_ARB_separate_shader_objects : enable

layout (location = 0) in vec2 vert_position;
layout (location = 1) in vec2 vert_tex_coord;
layout (location = 2) in vec2 vert_local;
layout (location = 3) in vec4 vert_shape;
layout (location = 0) out vec2 frag_tex_coord;
layout (location = 1) out vec2 frag_local;
layout (location = 2) flat out vec4 frag_shape;

layout (push_constant, std140) uniform DrawInfo {
    mat4 modelview_matrix;
    vec4 i_resolution;
    vec4 o_resolution;
    int screen_id_l;
    int screen_id_r;
    int layer;
};

void main() {
    vec4 position = vec4(vert_position, 0.0, 1.0) * modelview_matrix;
    gl_Position = vec4(position.x, position.y, 0.0, 1.0);
    frag_tex_coord = vert_tex_coord;
    frag_local = vert_local;
    frag_shape = vert_shape;
}
