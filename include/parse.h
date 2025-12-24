#pragma once
#include "stdint.h"
int tokenize(char *line, char **argv, int argv_max);
int streq(const char *a, const char *b);
int is_space(char c);
int hexval(char c);
int parse_u32(const char *s, uint32_t *out);