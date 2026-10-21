#include "mars_glyph.h"
#include "font_cn.h"
#include <string.h>

const uint8_t *mars_ascii_glyph(char c)
{
    static const uint8_t blank[7] = {0};
    static const uint8_t colon[7] = {0, 4, 4, 0, 4, 4, 0};
    static const uint8_t dash[7] = {0, 0, 0, 31, 0, 0, 0};
    static const uint8_t asterisk[7] = {0, 4, 21, 14, 21, 4, 0};
    static const uint8_t dot[7] = {0, 0, 0, 0, 0, 12, 12};
    static const uint8_t underscore[7] = {0, 0, 0, 0, 0, 0, 31};
    static const uint8_t at[7] = {14, 17, 23, 21, 23, 16, 14};
    static const uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14},
    };
    static const uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14},
        {7,2,2,2,18,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4}, {17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
    };
    static const uint8_t lowercase[26][7] = {
        {0,0,14,1,15,17,15}, {16,16,30,17,17,17,30}, {0,0,14,17,16,17,14},
        {1,1,15,17,17,17,15}, {0,0,14,17,31,16,14}, {6,9,8,28,8,8,8},
        {0,15,17,17,15,1,14}, {16,16,30,17,17,17,17}, {4,0,12,4,4,4,14},
        {2,0,6,2,2,18,12}, {16,16,18,20,24,20,18}, {12,4,4,4,4,4,14},
        {0,0,26,21,21,21,21}, {0,0,30,17,17,17,17}, {0,0,14,17,17,17,14},
        {0,30,17,17,30,16,16}, {0,15,17,17,15,1,1}, {0,0,22,25,16,16,16},
        {0,0,15,16,14,1,30}, {8,8,28,8,8,9,6}, {0,0,17,17,17,19,13},
        {0,0,17,17,17,10,4}, {0,0,17,17,21,21,10}, {0,0,17,10,4,10,17},
        {0,17,17,17,15,1,14}, {0,0,31,2,4,8,31},
    };
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= 'a' && c <= 'z') return lowercase[c - 'a'];
    if (c == ':') return colon;
    if (c == '-') return dash;
    if (c == '*') return asterisk;
    if (c == '.') return dot;
    if (c == '_') return underscore;
    if (c == '@') return at;
    /* Password entry needs every printable ASCII symbol; preserve existing glyphs. */
    static const struct { unsigned char code; uint8_t rows[7]; } punctuation[] = {
        {33, {4,4,4,4,4,0,4}},
        {34, {10,10,10,0,0,0,0}},
        {35, {10,31,10,10,31,10,0}},
        {36, {4,15,20,14,5,30,4}},
        {37, {24,25,2,4,8,19,3}},
        {38, {12,18,20,8,21,18,13}},
        {39, {4,4,8,0,0,0,0}},
        {40, {2,4,8,8,8,4,2}},
        {41, {8,4,2,2,2,4,8}},
        {43, {0,4,4,31,4,4,0}},
        {44, {0,0,0,0,0,4,8}},
        {47, {1,2,2,4,8,8,16}},
        {59, {0,4,0,0,4,4,8}},
        {60, {1,2,4,8,4,2,1}},
        {61, {0,0,31,0,31,0,0}},
        {62, {16,8,4,2,4,8,16}},
        {63, {14,17,1,2,4,0,4}},
        {91, {14,8,8,8,8,8,14}},
        {92, {16,8,8,4,2,2,1}},
        {93, {14,2,2,2,2,2,14}},
        {94, {4,10,17,0,0,0,0}},
        {96, {8,4,2,0,0,0,0}},
        {123, {2,4,4,8,4,4,2}},
        {124, {4,4,4,4,4,4,4}},
        {125, {8,4,4,2,4,4,8}},
        {126, {0,0,9,22,0,0,0}},
    };
    for (size_t i=0; i<sizeof(punctuation)/sizeof(punctuation[0]); ++i)
        if ((unsigned char)c==punctuation[i].code) return punctuation[i].rows;
    return blank;
}


int mars_glyph_copy(uint32_t cp, uint8_t *rows, size_t capacity, uint32_t *width)
{
    if (!rows || !width || capacity < 32) return -1;
    memset(rows, 0, 32);
    *width = cp < 128 ? 6 : 16;
    if (cp < 128) {
        const uint8_t *source = mars_ascii_glyph((char)cp);
        for (int y=0; y<7; ++y) rows[(y+4)*2] = source[y] << 3;
        /* Unsupported printable punctuation gets an explicit marker. */
        if (cp != ' ' && cp >= 32) {
            unsigned present = 0;
            for (int y=0; y<32; ++y) present |= rows[y];
            if (!present) {
                static const uint8_t question[7]={14,17,1,2,4,0,4};
                for (int y=0; y<7; ++y) rows[(y+4)*2]=question[y]<<3;
                return 1;
            }
        }
        return 0;
    }
    const uint8_t *source = font_cn_glyph(cp);
    if (source) { memcpy(rows, source, 32); return 0; }
    /* Missing/emoji glyph remains visible, and never exposes font pointers. */
    for (int y=1; y<15; ++y) {
        uint16_t line=(y==1 || y==14)?0x7ffe:0x4002;
        if (y>=5 && y<=10) line |= (uint16_t)(0x2000u >> (y-5));
        rows[y*2]=(uint8_t)(line>>8); rows[y*2+1]=(uint8_t)line;
    }
    return 1;
}
