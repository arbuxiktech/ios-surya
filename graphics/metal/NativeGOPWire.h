/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_GOP_WIRE_H
#define NATIVE_GOP_WIRE_H

#include <stdint.h>

#define NGP_VERSION 1
#define NGP_BGRX8888 1

#define NGP_METHOD_MODE 1
#define NGP_METHOD_WAIT_SWAP 2
#define NGP_METHOD_COMPLETE_SWAP 3

struct NGPMode {
    uint32_t version;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t bytes;
};

struct NGPWait {
    uint64_t timeout_ms;
};

struct NGPSwap {
    uint64_t sequence;
    uint64_t address;
    uint64_t bytes;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
};

struct NGPComplete {
    uint64_t sequence;
    uint32_t result;
    uint32_t reserved;
};

#endif /* NATIVE_GOP_WIRE_H */
