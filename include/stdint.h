#pragma once

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef signed long long int64_t;
typedef unsigned long long uint64_t;

typedef int8_t int_least8_t;
typedef int16_t int_least16_t;
typedef int32_t int_least32_t;
typedef int64_t int_least64_t;
typedef uint8_t uint_least8_t;
typedef uint16_t uint_least16_t;
typedef uint32_t uint_least32_t;
typedef uint64_t uint_least64_t;

typedef int32_t intptr_t;
typedef uint32_t uintptr_t;

typedef int64_t intmax_t;
typedef uint64_t uintmax_t;

/* compile-time size checks (C11 or GCC/Clang) */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(int8_t) == 1, "int8_t must be 1 byte");
_Static_assert(sizeof(int16_t) == 2, "int16_t must be 2 bytes");
_Static_assert(sizeof(int32_t) == 4, "int32_t must be 4 bytes");
_Static_assert(sizeof(int64_t) == 8, "int64_t must be 8 bytes");
_Static_assert(sizeof(void *) == 4, "pointer must be 32-bit");
#else
typedef char _assert_int8_t_size[(sizeof(int8_t) == 1) ? 1 : -1];
typedef char _assert_int16_t_size[(sizeof(int16_t) == 2) ? 1 : -1];
typedef char _assert_int32_t_size[(sizeof(int32_t) == 4) ? 1 : -1];
typedef char _assert_int64_t_size[(sizeof(int64_t) == 8) ? 1 : -1];
typedef char _assert_ptr_size[(sizeof(void *) == 4) ? 1 : -1];
#endif

