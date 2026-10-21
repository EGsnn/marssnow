#pragma once
#include <stddef.h>
#include <stdint.h>

/* Copies one system-font glyph into caller-owned 32 bytes: sixteen rows,
 * MSB-first, two bytes per row. Width is 6 (ASCII) or 16 (Chinese/fallback).
 * 0: glyph, 1: visible fallback, -1: invalid buffer. No font pointers escape.
 * capacity must be >=32; bytes beyond the first 32 are untouched. */
int mars_glyph_copy(uint32_t codepoint, uint8_t *rows, size_t capacity, uint32_t *width);

/* Internal graphics helper, intentionally absent from dynamic app exports. */
const uint8_t *mars_ascii_glyph(char c);
