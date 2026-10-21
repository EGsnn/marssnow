#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef enum { WIFI_KEYS_LETTERS, WIFI_KEYS_NUMBERS, WIFI_KEYS_SYMBOLS } wifi_keys_mode_t;
typedef enum { WIFI_KEY_NONE, WIFI_KEY_CHARACTER, WIFI_KEY_NEXT_PAGE } wifi_key_kind_t;
typedef struct { wifi_key_kind_t kind; char character; const char *label; } wifi_key_t;
enum {
    WIFI_KEYS_X=7, WIFI_KEYS_COLUMNS=5, WIFI_KEYS_WIDTH=58, WIFI_KEYS_PITCH_X=62,
    WIFI_KEYS_TOP=116, WIFI_KEYS_HEIGHT=36, WIFI_KEYS_PITCH_Y=40, WIFI_KEYS_ROWS=3,
    WIFI_KEYS_CONTROLS_Y=80, WIFI_KEYS_CONTROLS_HEIGHT=32,
    WIFI_KEYS_CONTROL_WIDTH=48, WIFI_KEYS_CONTROL_PITCH=52, WIFI_KEYS_CONTROL_COUNT=6,
};
static inline int wifi_keys_cell_hit(int x,int y)
{
    if (x<WIFI_KEYS_X || y<WIFI_KEYS_TOP) return -1;
    x-=WIFI_KEYS_X; y-=WIFI_KEYS_TOP;
    int col=x/WIFI_KEYS_PITCH_X, row=y/WIFI_KEYS_PITCH_Y;
    if (col>=WIFI_KEYS_COLUMNS || row>=WIFI_KEYS_ROWS
        || x%WIFI_KEYS_PITCH_X>=WIFI_KEYS_WIDTH || y%WIFI_KEYS_PITCH_Y>=WIFI_KEYS_HEIGHT) return -1;
    return row*WIFI_KEYS_COLUMNS+col;
}
static inline int wifi_keys_control_hit(int x,int y)
{
    if (x<WIFI_KEYS_X || y<WIFI_KEYS_CONTROLS_Y || y>=WIFI_KEYS_CONTROLS_Y+WIFI_KEYS_CONTROLS_HEIGHT) return -1;
    x-=WIFI_KEYS_X;
    int col=x/WIFI_KEYS_CONTROL_PITCH;
    return col<WIFI_KEYS_CONTROL_COUNT && x%WIFI_KEYS_CONTROL_PITCH<WIFI_KEYS_CONTROL_WIDTH ? col : -1;
}
static inline wifi_key_t wifi_keys_key(wifi_keys_mode_t mode,unsigned page,bool upper,int cell)
{
    wifi_key_t empty={WIFI_KEY_NONE,0,NULL};
    if (cell<0 || cell>=15) return empty;
    if (mode==WIFI_KEYS_NUMBERS) {
        const char *numbers="1234567890-.@ _";
        return (wifi_key_t){WIFI_KEY_CHARACTER,numbers[cell],cell==13 ? "空格" : NULL};
    }
    if (mode==WIFI_KEYS_LETTERS) {
        /* Preserve familiar QWERTY rows, split at their left/right halves. */
        static const char *const halves[]={"qwertasdfgzxcvb","yuiophjkl nm._@"};
        char ch=halves[page%2][cell];
        if (upper && ch>='a' && ch<='z') ch=ch-'a'+'A';
        return (wifi_key_t){WIFI_KEY_CHARACTER,ch,ch==' ' ? "空格" : NULL};
    }
    if (cell==13) return (wifi_key_t){WIFI_KEY_CHARACTER,' ',"空格"};
    if (mode==WIFI_KEYS_SYMBOLS) {
        static const char symbols[]="!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
        page%=3;
        if (cell==14) return (wifi_key_t){WIFI_KEY_NEXT_PAGE,0,page==2 ? "1/3" : page==1 ? "3/3" : "2/3"};
        unsigned index=page*13+cell;
        if (index<sizeof(symbols)-1) return (wifi_key_t){WIFI_KEY_CHARACTER,symbols[index],NULL};
    }
    return empty;
}
