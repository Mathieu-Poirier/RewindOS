#include "../../include/parse.h"

int streq(const char *a, const char *b)
{
        while (*a && *b)
        {
                if (*a != *b)
                        return 0;
                a++;
                b++;
        }
        return (*a == 0 && *b == 0);
}

int tokenize(char *line, char **argv, int argv_max)
{
        char *p = line;
        int argc = 0;

        while (*p)
        {
                while (*p == ' ' || *p == '\t')
                        p++;
                if (*p == '\0')
                        break;

                if (argc < argv_max)
                        argv[argc++] = p;

                while (*p && *p != ' ' && *p != '\t')
                        p++;

                if (*p)
                {
                        *p = '\0';
                        p++;
                }
        }

        return argc;
}

int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int hexval(char c)
{
        if (c >= '0' && c <= '9')
                return c - '0';
        if (c >= 'a' && c <= 'f')
                return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F')
                return 10 + (c - 'A');
        return -1;
}

int parse_u32(const char *s, uint32_t *out)
{
        if (!s || !*s)
                return 0;

        /* base detect */
        int base = 10;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        {
                base = 16;
                s += 2;
        }

        uint32_t v = 0;
        int any = 0;

        if (base == 16)
        {
                for (; *s && !is_space(*s); s++)
                {
                        int h = hexval(*s);
                        if (h < 0)
                                return 0;
                        v = (v << 4) | (uint32_t)h;
                        any = 1;
                }
        }
        else
        {
                for (; *s && !is_space(*s); s++)
                {
                        if (*s < '0' || *s > '9')
                                return 0;
                        v = v * 10u + (uint32_t)(*s - '0');
                        any = 1;
                }
        }

        if (!any)
                return 0;
        *out = v;
        return 1;
}