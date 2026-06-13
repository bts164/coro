#pragma once

/* lwIP platform port for Linux host (NO_SYS test builds).
 * Provides the compiler/type definitions lwIP needs from the port layer. */

#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/* Integer types */
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uintptr_t mem_ptr_t;

/* Printf format strings */
#define X8_F   "02" PRIx8
#define U16_F  PRIu16
#define S16_F  PRId16
#define X16_F  PRIx16
#define U32_F  PRIu32
#define S32_F  PRId32
#define X32_F  PRIx32
#define SZT_F  "zu"

/* Compiler hints */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT   __attribute__((packed))

/* Diagnostics */
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { assert(x); } while (0)

/* Byte order — Linux x86/x86_64 is little-endian */
#define BYTE_ORDER LITTLE_ENDIAN
