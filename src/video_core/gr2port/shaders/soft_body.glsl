// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The SoftBody jobs and effector spheres of the scene packet, laid out as Scene::SoftBodyJob and
// Scene::Effectors, the renderer's effector grid, and the state of an instance's particles.

layout(push_constant) uniform Push {
    uint64_t jobs;
    uint64_t effectors;
    uint64_t grid;
};

// 6 x 6 x 10 cells, each listing up to 127 sphere indices, then -1.
const uint GridCells = 360u;
const uint GridEntries = 128u;
const uint JobBytes = 384u;

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SoftBodyJob {
    uint64_t skinned;
    uint64_t particles;
    uint64_t constraints;
    uint64_t map;
    uint64_t state;
    uint64_t destination;
    uint reset;
    uint padding[3];
    uvec4 constants[15];
    uvec4 write[5];
};

// Two vec4s a sphere: {center xyz, radii (outer and inner half floats)}, {..., mask and strength
// half float}.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Effectors {
    mat4 view;
    float depth;
    float start;
    float slice;
    uint count;
    float projection_x;
    float projection_y;
    uint unused[2];
    uvec4 spheres[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) buffer Grid {
    uint entries[];
};

// An instance's particles: a flag the write pass sets once they hold positions, then {position,
// previous position} per particle. The simulate pass reads positions that other invocations
// store while it runs, as the game's program does.
layout(buffer_reference, std430, buffer_reference_align = 16) coherent buffer State {
    uint valid;
    uint unused[7];
    vec4 particles[];
};

// The workgroup's job.
SoftBodyJob Job() {
    return SoftBodyJob(jobs + uint64_t(gl_WorkGroupID.y) * JobBytes);
}

uint Word(SoftBodyJob job, uint i) {
    return job.constants[i / 4u][i % 4u];
}

float Constant(SoftBodyJob job, uint i) {
    return uintBitsToFloat(Word(job, i));
}

// The three constants from i, a multiple of 4.
vec3 ConstantVector(SoftBodyJob job, uint i) {
    return uintBitsToFloat(job.constants[i / 4u].xyz);
}

// The matrix of the 16 constants from i, a multiple of 4: the game's matrices are column-major.
mat4 ConstantMatrix(SoftBodyJob job, uint i) {
    return mat4(uintBitsToFloat(job.constants[i / 4u]), uintBitsToFloat(job.constants[i / 4u + 1u]),
                uintBitsToFloat(job.constants[i / 4u + 2u]),
                uintBitsToFloat(job.constants[i / 4u + 3u]));
}
