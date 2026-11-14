// Copyright 2022-2024 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#version 450 core
#extension GL_ARB_separate_shader_objects : enable

layout (location = 0) in vec2 frag_tex_coord;
layout (location = 1) in vec2 frag_local;
// The drawn rectangle's width, height, corner radius and opacity.
layout (location = 2) flat in vec4 frag_shape;
layout (location = 0) out vec4 color;

layout (push_constant, std140) uniform DrawInfo {
    mat4 modelview_matrix;
    vec4 i_resolution;
    vec4 o_resolution;
    int screen_id_l;
    int screen_id_r;
    int layer;
    int reverse_interlaced;
};

layout (set = 0, binding = 0) uniform sampler2D screen_textures[3];

vec4 GetScreen(int screen_id) {
#ifdef ARRAY_DYNAMIC_INDEX
    return texture(screen_textures[screen_id], frag_tex_coord);
#else
    switch (screen_id) {
    case 0:
        return texture(screen_textures[0], frag_tex_coord);
    case 1:
        return texture(screen_textures[1], frag_tex_coord);
    case 2:
        return texture(screen_textures[2], frag_tex_coord);
    }
#endif
}

// The opacity of this fragment of a rectangle with rounded corners.
float Coverage() {
    if (frag_shape.z <= 0.0) {
        return frag_shape.w;
    }
    vec2 half_size = frag_shape.xy * 0.5;
    vec2 q = abs(frag_local - half_size) - half_size + frag_shape.z;
    float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - frag_shape.z;
    return frag_shape.w * clamp(0.5 - distance, 0.0, 1.0);
}

void main() {
    color = vec4(GetScreen(screen_id_l).rgb, Coverage());
}
