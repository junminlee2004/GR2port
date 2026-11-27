/* Recorded calls of game functions (src/cpu/record.c, docs/RECOMPILATION.md): the format shared by
 * the recorder in the game and tools/recomp/replay.c. Native layout (both sides are arm64 macOS).
 *
 * A file holds records one after another: a GuestRecHeader, the registers at the function's entry
 * (GuestRecRegs), then `events` events (`bytes` bytes; none when `failed`). An event is a
 * GuestRecEvent followed by `length` bytes: the memory (OBSERVE, WRITE) or a GuestRecRegs (CALL,
 * RETURN, EXIT).
 *
 * Epochs: the calls a function makes split its run into epochs (epoch k: after k calls returned).
 * OBSERVE gives memory as the function first saw it in its epoch: before any of its own writes
 * there. The memory a callee changed is observed again in the next epoch. WRITE gives memory the
 * function itself changed, after the instruction. */
#ifndef GUEST_CPU_RECORD_H
#define GUEST_CPU_RECORD_H
#include <stdint.h>

#define GUEST_REC_MAGIC 0x43524242u /* "BBRC" */
#define GUEST_REC_VERSION 1u

typedef struct {
    uint64_t r[16]; /* rax rcx rdx rbx rsp rbp rsi rdi r8-r15 */
    uint64_t rip, flags;
    uint64_t fs_base, gs_base, tcb;
    uint32_t mxcsr, pad;
    uint8_t v[16][32]; /* ymm0-15 */
} GuestRecRegs;

typedef struct {
    uint32_t magic, version;
    uint64_t offset;         /* image offset of the function */
    uint64_t image_base;
    uint64_t index;          /* calls of the function seen before this one */
    uint64_t return_address;
    uint64_t tcb_address;    /* where the recording thread kept its TCB pointer (gs-relative loads) */
    uint64_t instructions;   /* run by the function itself (callees not counted) */
    uint32_t events, calls;
    uint32_t racy;           /* bytes that changed under the function within an epoch (other threads) */
    uint32_t failed;         /* 0, or why the record is not usable (GuestRecFailure) */
    uint64_t bytes;          /* of the events */
} GuestRecHeader;

enum { GUEST_REC_OBSERVE = 1, GUEST_REC_WRITE = 2, GUEST_REC_CALL = 3, GUEST_REC_RETURN = 4, GUEST_REC_EXIT = 5 };
enum { GUEST_REC_OK = 0, GUEST_REC_UNSUPPORTED = 1, GUEST_REC_TOO_BIG = 2 };

typedef struct {
    uint8_t kind;
    uint8_t tail;   /* CALL: a tail call (the function ends when it returns) */
    uint16_t pad;
    uint32_t length;
    uint64_t address; /* OBSERVE, WRITE: memory; CALL: the target */
} GuestRecEvent;

#endif
