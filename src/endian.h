#pragma once

#if defined(__has_include_next) && __has_include_next(<endian.h>)
#include_next <endian.h>
#else
#include <stdint.h>

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#ifndef BYTE_ORDER
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define BYTE_ORDER BIG_ENDIAN
#else
#define BYTE_ORDER LITTLE_ENDIAN
#endif
#endif

#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN LITTLE_ENDIAN
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN BIG_ENDIAN
#endif
#ifndef __BYTE_ORDER
#define __BYTE_ORDER BYTE_ORDER
#endif

#define espeak_bswap16(x) __builtin_bswap16((uint16_t)(x))
#define espeak_bswap32(x) __builtin_bswap32((uint32_t)(x))
#define espeak_bswap64(x) __builtin_bswap64((uint64_t)(x))

#if BYTE_ORDER == BIG_ENDIAN
#define htobe16(x) (x)
#define htole16(x) espeak_bswap16(x)
#define be16toh(x) (x)
#define le16toh(x) espeak_bswap16(x)
#define htobe32(x) (x)
#define htole32(x) espeak_bswap32(x)
#define be32toh(x) (x)
#define le32toh(x) espeak_bswap32(x)
#define htobe64(x) (x)
#define htole64(x) espeak_bswap64(x)
#define be64toh(x) (x)
#define le64toh(x) espeak_bswap64(x)
#else
#define htobe16(x) espeak_bswap16(x)
#define htole16(x) (x)
#define be16toh(x) espeak_bswap16(x)
#define le16toh(x) (x)
#define htobe32(x) espeak_bswap32(x)
#define htole32(x) (x)
#define be32toh(x) espeak_bswap32(x)
#define le32toh(x) (x)
#define htobe64(x) espeak_bswap64(x)
#define htole64(x) (x)
#define be64toh(x) espeak_bswap64(x)
#define le64toh(x) (x)
#endif
#endif
