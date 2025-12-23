#pragma once
#include "stdint.h"

typedef uint32_t size_t;
typedef int32_t ssize_t;

#define NULL ((void *)0)
#define offsetof(type, member) ((size_t)&(((type *)0)->member))
