#include "text.h"
#include <stdint.h>

void to_ascii(const char *in, char *out, size_t n)
{
    static const char LATIN1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    size_t o = 0;
    const uint8_t *p = (const uint8_t *)in;
    while (*p && o + 1 < n) {
        uint32_t cp;
        int len;
        if (*p < 0x80)                { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
        else                          { cp = *p & 0x07; len = 4; }
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { len = i; cp = '?'; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        p += len;

        const char *rep = nullptr;
        char one[2] = {0, 0};
        if (cp >= 0x20 && cp < 0x7F)          { one[0] = (char)cp; rep = one; }
        else if (cp >= 0xC0 && cp <= 0xFF)    { one[0] = LATIN1[cp - 0xC0]; rep = one; }
        else if (cp == 0x2018 || cp == 0x2019) rep = "'";
        else if (cp == 0x201C || cp == 0x201D) rep = "\"";
        else if (cp == 0x2013 || cp == 0x2014) rep = "-";
        else if (cp == 0x2026)                 rep = "...";
        for (; rep && *rep && o + 1 < n; rep++) out[o++] = *rep;
    }
    out[o] = 0;
}
