/* output_owner：输出通路独占所有权状态机实现（纯 C，见 output_owner.h） */

#include "output_owner.h"

void output_owner_init(output_owner_t *o)
{
    if (o == NULL) {
        return;
    }
    o->claimed = false;
    o->state_before = false;
}

bool output_owner_claim(output_owner_t *o, bool current_enabled)
{
    if (o == NULL) {
        return false;
    }
    if (o->claimed) {
        return false;   /* P1 单所有者：已被占用 */
    }
    o->claimed = true;
    o->state_before = current_enabled;
    return true;
}

bool output_owner_release(output_owner_t *o)
{
    if (o == NULL || !o->claimed) {
        return false;   /* 无 claim：什么都不动 */
    }
    o->claimed = false;
    /* 只恢复自己改变的：claim 前是关的 → 本所有者打开了它 → 需要关 */
    return !o->state_before;
}

bool output_owner_is_claimed(const output_owner_t *o)
{
    return (o != NULL) && o->claimed;
}
