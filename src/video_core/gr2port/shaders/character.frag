// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler texture_sampler;
layout(set = 0, binding = 2) uniform sampler clamp_sampler;
layout(set = 1, binding = 0) uniform sampler2DArrayShadow shadow_map;
layout(set = 1, binding = 2) uniform sampler2DShadow last_shadow_map;
layout(set = 2, binding = 0) uniform texture2D self_translucent_color;
layout(set = 2, binding = 1) uniform texture2D self_translucent_depth;

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Characters scene_characters;
    Decals scene_decals;
    Light light;
    vec2 target_inverse;
};

layout(location = 0) in vec3 view_position;
layout(location = 1) in vec4 normal;
layout(location = 2) in vec4 tangent;
layout(location = 3) in vec2 texcoord;
layout(location = 4) in vec2 decal_texcoord;
layout(location = 5) in vec2 model_xy;
layout(location = 6) in vec4 vertex_color;
layout(location = 7) flat in uint draw_index;

layout(location = 0) out vec4 color;

const vec2 FirstCascadeTaps[9] = vec2[9](vec2(6.7226e-4, 7.6843e-4), vec2(3.3259e-4, 3.0164e-4),
                                         vec2(-2.7996e-4, 8.6433e-4), vec2(-2.1857e-4, 1.2991e-4),
                                         vec2(-6.9220e-4, 4.6003e-4), vec2(-1.0455e-3, 3.3551e-6),
                                         vec2(-2.2026e-4, -6.5124e-4), vec2(2.0574e-4, -2.6155e-4),
                                         vec2(7.5692e-4, -8.9779e-5));
const vec2 SecondCascadeTaps[4] = vec2[4](vec2(1.3e-4, 3.64e-4), vec2(3.56e-4, 1.01e-4),
                                          vec2(-2.24e-4, 7.96e-5), vec2(2.02e-4, -4.23e-4));

vec4 Sample(uint handle, vec2 uv) {
    return texture(sampler2D(textures[nonuniformEXT(handle)], texture_sampler), uv);
}

float CascadeTap(uint cascade, vec3 coord, vec2 offset) {
    if (cascade == 2u) {
        return texture(last_shadow_map, vec3(coord.xy + offset, coord.z));
    }
    return texture(shadow_map, vec4(coord.xy + offset, float(cascade), coord.z));
}

// How much of the key light reaches the point: nine taps in the first cascade, four in the second
// and one in the last.
float KeyShadow() {
    const float depth = -view_position.z;
    const uint cascade = depth < light.split_depths.y ? 0u : depth < light.split_depths.z ? 1u : 2u;
    const vec4 clip = light.shadow_projection[cascade] * vec4(view_position, 1.0);
    const vec3 coord = clip.xyz / clip.w;
    if (cascade == 2u) {
        return CascadeTap(cascade, coord, vec2(0.0));
    }
    float sum = 0.0;
    if (cascade == 0u) {
        for (uint i = 0u; i < 9u; i++) {
            sum += CascadeTap(cascade, coord, FirstCascadeTaps[i]);
        }
        return sum / 9.0;
    }
    for (uint i = 0u; i < 4u; i++) {
        sum += CascadeTap(cascade, coord, SecondCascadeTaps[i]);
    }
    return 0.25 * sum;
}

// The share past a soft edge, by the game's smooth curve.
float SoftStep(float start, float end, float value) {
    const float t = clamp((value - start) / (end - start), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// The ambient light on a world-space normal: spherical harmonics in the game's seven constants.
vec3 AmbientOn(vec3 n) {
    const vec4 linear = vec4(n, 1.0);
    const vec4 quadratic = vec4(n.x * n.y, n.y * n.z, n.z * n.z, n.x * n.z);
    return vec3(dot(light.sh[0], linear), dot(light.sh[1], linear), dot(light.sh[2], linear)) +
           vec3(dot(light.sh[3], quadratic), dot(light.sh[4], quadratic),
                dot(light.sh[5], quadratic)) +
           light.sh[6].rgb * (n.x * n.x - n.y * n.y);
}

// The game's G2_CH_Main pixel programs without their point lights and self-translucency, in the
// order they run: bands of the toon light, ambient light, camera light, reflection, detail,
// highlight, rim.
void main() {
    Characters table = scene_characters;
    const uint index = scene_draws.draws[draw_index].material - 1u;
    const uint color_handle = scene_draws.draws[draw_index].texture;
    const uint normal_handle = table.characters[index].normal_map;
    const vec2 uv = texcoord;
    const vec4 color_map = Sample(color_handle, uv);
    float mask = color_map.a;
    if (table.characters[index].alpha_step != 0u) {
        const vec2 height_uv =
            table.characters[index].normal_offset + table.characters[index].normal_repeat * uv;
        const float height = pow(abs(Sample(normal_handle, height_uv).a), 2.2);
        if (table.characters[index].alpha_reference >= height * mask) {
            discard;
        }
        mask = 1.0;
    }
    // A screen door fades instances in, each with its own pattern.
    const vec2 punch = table.characters[index].punch_through;
    const float seed =
        256.0 * punch.x * (0.0771356 * gl_FragCoord.x + 0.00888381 * gl_FragCoord.y) + 1e-7;
    if (punch.y < fract(fract(abs(123456792.0 / seed)) * abs(seed))) {
        discard;
    }

    const vec3 detail =
        Sample(table.characters[index].detail, table.characters[index].detail_repeat * uv).rgb;
    const float detail_value = (detail.r + detail.g + detail.b) / 3.0;
    const float detail_mask =
        max(1.0 - table.characters[index].detail_mask * detail_value, 0.0);
    const float shadow = KeyShadow();

    // The normal map's strength follows the detail map.
    const float strength =
        table.characters[index].detail_border * (100.0 * detail_value - 50.0) + 1.0;
    // A material without a normal map holds a constant texture.
    vec4 normal_texel = vec4(128.0 / 255.0, 128.0 / 255.0, 1.0, 1.0);
    if (normal_handle != 0u) {
        normal_texel = Sample(normal_handle, uv);
    }
    const float roughness_map = normal_texel.a * vertex_color.x;
    const vec3 surface_normal = normalize(
        vec3(strength * (2.0 * normal_texel.rg - 1.0), normal_texel.b));
    const vec3 t = normalize(tangent.xyz);
    const vec3 base_normal = normalize(normal.xyz);
    const vec3 b = cross(base_normal, t) * sign(tangent.w);
    const vec3 n = normalize(t * surface_normal.x + b * surface_normal.y +
                             base_normal * surface_normal.z);

    // Bands of the toon light, over half Lambert.
    const vec3 travel = table.characters[index].light_direction.xyz;
    const float half_lambert = 0.5 - 0.5 * dot(travel, n);
    const float facing = max(n.z, 0.0);
    const float facing8 = pow(facing, 8.0);
    const float highlight_center = 1.0 + table.characters[index].highlight_fresnel * facing8 -
                                   (1.0 - table.characters[index].highlight_border) * roughness_map;
    const float highlight_soft =
        max(table.characters[index].highlight_smooth * roughness_map, 1e-4);
    const float highlight = SoftStep(highlight_center - highlight_soft,
                                     highlight_center + highlight_soft, half_lambert);
    const float diffuse =
        SoftStep(table.characters[index].diffuse_border - table.characters[index].diffuse_smooth,
                 table.characters[index].diffuse_border + table.characters[index].diffuse_smooth,
                 half_lambert);
    const float darkened = table.characters[index].mask_density * (1.0 - shadow);
    const vec3 ambient_color = table.characters[index].ambient_color.rgb;
    const vec3 tone = mix(mix(table.characters[index].diffuse_color.rgb, ambient_color, darkened),
                          mix(table.characters[index].highlight_color.rgb, ambient_color, darkened),
                          highlight);
    const vec3 light_color = table.characters[index].light_color.rgb;
    const float light_scale = table.characters[index].light_color.a;
    const vec3 lit_tone = light_color * tone;
    const vec3 line = mix(0.5 * (ambient_color + lit_tone), ambient_color * (0.89 - 0.39 * shadow),
                          table.characters[index].diffuse_line);
    const vec3 toon = mix(mix(lit_tone, line, min(2.0 - 2.0 * diffuse, 1.0)), ambient_color,
                          2.0 * max(0.5 - diffuse, 0.0));

    // The surface colour, drawn to the gradation where the vertex colour's z is low, with its
    // decal. A material without a decal file holds a white constant texture.
    const vec4 gradation = table.characters[index].gradation_color;
    vec3 albedo = mix(color_map.rgb * table.characters[index].color_scale.rgb, gradation.rgb,
                      gradation.a * (1.0 - vertex_color.z));
    vec4 decal = vec4(1.0);
    const uint decal_index = scene_draws.draws[draw_index].decal;
    if (decal_index != 0u) {
        const uint handle = scene_decals.decals[decal_index - 1u].texture;
        decal = texture(sampler2D(textures[nonuniformEXT(handle)], clamp_sampler), decal_texcoord);
    }
    albedo *= mix(vec3(1.0), mix(decal.rgb, gradation.rgb, gradation.a), decal.a * mask);

    // In shadow, the shadow colour carries a share of the surface's colour.
    const vec3 shadow_color = table.characters[index].shadow_color.rgb;
    const vec3 in_shadow =
        shadow_color * (1.0 + table.characters[index].shadow_gradation * (albedo - 1.0));
    const float lit_share = SoftStep(
        clamp(table.characters[index].shadow_border - table.characters[index].shadow_smooth, 0.0,
              1.0),
        table.characters[index].shadow_border + table.characters[index].shadow_smooth,
        half_lambert + table.characters[index].shadow_fresnel * n.z * n.z);
    vec3 result = light_scale * mix(in_shadow, albedo * toon, lit_share);
    result += table.characters[index].ibl_blend * albedo *
              AmbientOn(normalize(mat3(table.characters[index].view_inverse) * n));

    // The camera's light.
    const float distance_to_eye = length(view_position);
    const vec3 to_eye = -view_position / distance_to_eye;
    const float camera_wrap = clamp(3.3333333 * (dot(n, to_eye) + 1.0) - 2.3333333, 0.0, 1.0);
    const float camera_reach =
        min(max(distance_to_eye - table.characters[index].camera_light_inner, 0.0) /
                (table.characters[index].camera_light_outer -
                 table.characters[index].camera_light_inner),
            1.0);
    const float camera_share =
        camera_wrap * camera_wrap * (3.0 - 2.0 * camera_wrap) *
        (1.0 - pow(abs(camera_reach), table.characters[index].camera_light_attenuation));
    const float camera_facing =
        table.characters[index].camera_light_directivity * (max(dot(travel, to_eye), 0.0) - 1.0) +
        1.0;
    result +=
        camera_facing * camera_share * table.characters[index].camera_light_color.rgb * albedo;
    result = max(result, vec3(0.0));

    // The reflection map, on the model or on the screen.
    const vec2 distortion = table.characters[index].reflection_distortion * n.xy;
    const int reflection_mode = table.characters[index].reflection_mode;
    const float repeat = table.characters[index].reflection_repeat;
    const vec2 reflection_uv =
        reflection_mode == 0
            ? table.characters[index].reflection_offset + repeat * (model_xy + distortion)
            : repeat * (distortion + gl_FragCoord.xy * target_inverse);
    // A material without a reflection map holds a black constant texture.
    const uint reflection_map = table.characters[index].reflection;
    const vec3 reflection =
        reflection_map != 0u ? Sample(reflection_map, reflection_uv).rgb : vec3(0.0);
    const float reflection_share =
        light_scale * table.characters[index].reflection_blend * mask * detail_mask;
    if (reflection_mode == 1) {
        result += reflection_share * (2.0 * reflection * result - result);
    } else {
        result += reflection_share * reflection;
    }

    // The detail map overlays the colour, less so in the light.
    result += table.characters[index].detail_blend * vertex_color.w * (1.0 - 0.5 * diffuse) *
              (2.0 * detail * result - result);

    // The highlight and the rim.
    const float half_z = normalize(n - travel).z;
    const float highlight_value =
        pow(max(half_z, 0.0), 40.0) -
        2.0 * table.characters[index].specular_fresnel * pow(facing8, 4.0);
    const float specular = SoftStep(
        table.characters[index].specular_border - table.characters[index].specular_smooth,
        table.characters[index].specular_border + table.characters[index].specular_smooth,
        highlight_value);
    const vec3 specular_color = table.characters[index].specular_color.rgb * diffuse;
    result += min(result, specular_color) * diffuse * specular * detail_mask * vertex_color.x;
    const float rim_light =
        min(table.characters[index].rim_lighting * max(travel.z, 0.0), 1.0) * normal.w;
    const float rim =
        SoftStep(table.characters[index].rim_border - table.characters[index].rim_smooth,
                 table.characters[index].rim_border + table.characters[index].rim_smooth,
                 rim_light * max(1.0 - n.z, 0.0));
    result += (1.8 * result + 0.2) * light_color * table.characters[index].rim_intensity * rim;
    if (scene_draws.draws[draw_index].vertex_color != 0u) {
        result = mix(shadow_color * table.characters[index].vertex_shadow, result, vertex_color.y);
    }

    // The colour or alpha of a glow.
    const uint bloom_mode = table.characters[index].bloom_fresnel_mode;
    if (bloom_mode != 0u) {
        const float glow = SoftStep(
            table.characters[index].bloom_fresnel_border -
                table.characters[index].bloom_fresnel_smooth,
            table.characters[index].bloom_fresnel_border +
                table.characters[index].bloom_fresnel_smooth,
            1.0 - n.z * n.z);
        if (bloom_mode == 1u) {
            result *= glow;
        } else {
            mask *= glow;
        }
    }

    // What SelfTranslucent drew shows through the skin, by how much skin lies over it.
    const float translucency = table.characters[index].self_translucency;
    if (translucency > 0.0) {
        const ivec2 pixel = ivec2(gl_FragCoord.xy);
        const vec3 inside =
            table.characters[index].self_translucency_scale *
            texelFetch(sampler2D(self_translucent_color, clamp_sampler), pixel, 0).rgb;
        // Its reverse depth as the game's depth, then as a distance.
        const float game_depth =
            1.0 - texelFetch(sampler2D(self_translucent_depth, clamp_sampler), pixel, 0).r;
        const vec2 inverse_z = table.characters[index].inverse_z;
        const float inside_distance = 1.0 / (inverse_z.y + inverse_z.x * (2.0 * game_depth - 1.0));
        const float start = smoothstep(
            0.0, 1.0, inside_distance / table.characters[index].self_translucency_start);
        const float skin = smoothstep(0.0, 1.0,
                                      (inside_distance + view_position.z) / start /
                                          table.characters[index].self_translucency_depth);
        const float share =
            translucency * pow(max(n.z, 0.0), table.characters[index].self_translucency_power) *
            clamp(1.0 + table.characters[index].self_translucency_offset - skin, 0.0, 1.0);
        result = table.characters[index].self_translucency_mode != 0u ? result + share * inside
                                                                      : mix(result, inside, share);
    }
    color = vec4(result, mask);
}
