// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

// The SoftBody jobs of the scene packet, laid out as Scene::SoftBodyJob, and the state of an
// instance's particles.

#include "effectors.glsl"

layout(push_constant) uniform Push {
    uint64_t jobs;
    uint64_t effectors;
    uint64_t grid;
};

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
