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

// Anaglyph Red-Cyan shader based on Dubois algorithm
// Constants taken from the paper:
// "Conversion of a Stereo Pair to Anaglyph with
// the Least-Squares Projection Method"
// Eric Dubois, March 2009
const mat3 l = mat3( 0.437, 0.449, 0.164,
              -0.062,-0.062,-0.024,
              -0.048,-0.050,-0.017);
const mat3 r = mat3(-0.011,-0.032,-0.007,
               0.377, 0.761, 0.009,
              -0.026,-0.093, 1.234);

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
    vec4 color_tex_l = GetScreen(screen_id_l);
    vec4 color_tex_r = GetScreen(screen_id_r);
    color = vec4(color_tex_l.rgb*l+color_tex_r.rgb*r, Coverage());
}
