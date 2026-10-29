#pragma once
#include <stddef.h>

// The display fonts only cover ASCII, so fold accented Latin letters and
// curly punctuation down to plain ASCII and drop anything else.
void to_ascii(const char *in, char *out, size_t n);
