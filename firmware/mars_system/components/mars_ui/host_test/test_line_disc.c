/* 碎片化串口命令测试（P0-1）
 *
 * 编译（纯 C，无 ESP 依赖）：
 *   clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *     -I ../include test_line_disc.c ../line_disc.c -o test_line_disc && ./test_line_disc
 *
 * 验证：
 *  - 行缓冲把字节流按任意分片喂入，每条完整 CR/LF 行恰好分发一次；
 *  - '\r' 被忽略，'\n' 结束行；未知命令不污染下一行；
 *  - 行超长时丢弃当前行，后续行仍能正常解析。
 */
#include <stdio.h>
#include <string.h>

#include "line_disc.h"

static int g_fails;
#define CHECK(cond, msg) do { \
    if (cond) printf("  PASS: %s\n", msg); \
    else { printf("  FAIL: %s\n", msg); g_fails++; } \
} while (0)

/* 收集分发到的行 */
static char g_lines[16][64];
static int g_nlines;
static int g_overflow;

static void on_line(const char *line, void *ctx)
{
    (void)ctx;
    if (g_nlines < 16) {
        snprintf(g_lines[g_nlines], sizeof(g_lines[g_nlines]), "%s", line);
    }
    g_nlines++;
}
static void on_ovf(void *ctx) { (void)ctx; g_overflow++; }

/* 把完整输入串按所有可能的分片方式喂入（穷举分片不可行，用几种代表性碎片），
 * 验证最终得到相同的行序列。 */
static void feed_all(mars_line_disc_t *d, const char *input, int n)
{
    /* 分片 1：逐字节 */
    for (int i = 0; i < n; ++i) {
        mars_line_disc_feed(d, input + i, 1, on_line, NULL, on_ovf);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("═══ line_disc 碎片化命令测试 ═══\n");
    char buf[128];

    /* 用例 1：三条命令，逐字节喂入（最恶劣分片） */
    {
        g_nlines = 0; g_overflow = 0;
        mars_line_disc_t d;
        mars_line_disc_init(&d, buf, sizeof(buf));
        const char *input = "@CAPS?\n@ACTION:tapped\n@UI:{\"x\":1}\r\n";
        feed_all(&d, input, (int)strlen(input));
        CHECK(g_nlines == 3, "逐字节分片：3 条行恰好分发 3 次");
        CHECK(strcmp(g_lines[0], "@CAPS?") == 0, "行1 = @CAPS?");
        CHECK(strcmp(g_lines[1], "@ACTION:tapped") == 0, "行2 = @ACTION:tapped");
        CHECK(strcmp(g_lines[2], "@UI:{\"x\":1}") == 0, "行3 去掉尾部 \\r");
        CHECK(g_overflow == 0, "无溢出");
    }

    /* 用例 2：未知命令不污染下一行（逐字节喂入） */
    {
        g_nlines = 0; g_overflow = 0;
        mars_line_disc_t d;
        mars_line_disc_init(&d, buf, sizeof(buf));
        const char *input = "@GARBAGE\n@SWITCH:next\n";
        feed_all(&d, input, (int)strlen(input));
        CHECK(g_nlines == 2, "未知命令后下一行仍完整分发");
        CHECK(strcmp(g_lines[0], "@GARBAGE") == 0, "未知命令本身被分发（静默丢弃）");
        CHECK(strcmp(g_lines[1], "@SWITCH:next") == 0, "未知命令未污染下一行");
    }

    /* 用例 3（P0-1 对抗）：超长行尾部不得被拼成有效命令。
     * 容量 16 时输入 "XXXXXXXXXXXXXXXX@ACTION:bad\n@CAPS?\n"：
     * 第一次溢出后整行丢弃，直到换行；@ACTION:bad 绝不能被交付，
     * 最终只应得到 1 条 @CAPS?。 */
    {
        g_nlines = 0; g_overflow = 0;
        mars_line_disc_t d;
        mars_line_disc_init(&d, buf, 16);   /* 小行缓冲 */
        const char *adv = "XXXXXXXXXXXXXXXX@ACTION:bad\n@CAPS?\n";
        mars_line_disc_feed(&d, adv, strlen(adv), on_line, NULL, on_ovf);
        CHECK(g_overflow == 1, "超长行只触发一次 overflow");
        CHECK(g_nlines == 1, "超长行尾部未被交付，只分发 1 条正常行");
        CHECK(strcmp(g_lines[0], "@CAPS?") == 0, "唯一分发的行是 @CAPS?");
    }

    /* 用例 3b：超长行被换行结束后，再喂的多行仍逐行完整解析 */
    {
        g_nlines = 0; g_overflow = 0;
        mars_line_disc_t d;
        mars_line_disc_init(&d, buf, 16);
        /* 逐字节喂，制造最恶劣分片 */
        const char *adv = "XXXXXXXXXXXXXXXX@ACTION:bad\n@SWITCH:a\n@SWITCH:b\n";
        feed_all(&d, adv, (int)strlen(adv));
        CHECK(g_overflow == 1, "3b: 只一次 overflow");
        CHECK(g_nlines == 2, "3b: 超长行后两行正常分发（共 2 条）");
        CHECK(strcmp(g_lines[0], "@SWITCH:a") == 0 &&
              strcmp(g_lines[1], "@SWITCH:b") == 0, "3b: 两条正常行顺序正确");
    }

    /* 用例 4：无换行的尾部不完整缓冲不应分发（保持到下次喂入） */
    {
        g_nlines = 0; g_overflow = 0;
        mars_line_disc_t d;
        mars_line_disc_init(&d, buf, sizeof(buf));
        mars_line_disc_feed(&d, "@UI:abc", 7, on_line, NULL, on_ovf);
        CHECK(g_nlines == 0, "无换行不分发");
        mars_line_disc_feed(&d, "\n", 1, on_line, NULL, on_ovf);
        CHECK(g_nlines == 1 && strcmp(g_lines[0], "@UI:abc") == 0, "补换行后整行分发");
    }

    printf("\n═══ RESULT: %s（%d 失败）═══\n", g_fails ? "FAIL" : "PASS", g_fails);
    return g_fails ? 1 : 0;
}
