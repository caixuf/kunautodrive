#include "behavior_route_goal.h"

#include <cassert>
#include <cstdio>

static int g_fails = 0;

static void expect(bool ok, const char* msg) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", msg);
    g_fails++;
}

int main() {
    using behavior_route_goal::next_lane;

    /* 4 车道双向：本向 [2,3]；当前 2 目标 3 → 右移一条 */
    auto d = next_lane(2, 3, 4, false, false, false);
    expect(d.trigger && d.lane == 3, "two-way 2->3 right one");

    /* 当前 3 目标 2 → 左移一条 */
    d = next_lane(3, 2, 4, false, false, false);
    expect(d.trigger && d.lane == 2, "two-way 3->2 left one");

    /* 已在目标车道 → 不触发 */
    d = next_lane(2, 2, 4, false, false, false);
    expect(!d.trigger, "already at goal");

    /* 目标在对向半幅 → 拒绝（不横穿中心线） */
    d = next_lane(2, 0, 4, false, false, false);
    expect(!d.trigger, "two-way goal in oncoming half rejected");

    /* 单向 4 车道：目标 0 合法 */
    d = next_lane(1, 0, 4, true, false, false);
    expect(d.trigger && d.lane == 0, "one-way 1->0 legal");

    /* 单向 8 车道：4 -> 7 每次只走一步 */
    d = next_lane(4, 7, 8, true, false, false);
    expect(d.trigger && d.lane == 5, "one-way 8 lane step 4->5");

    /* 双向 8 车道：4 -> 7 每次只走一步 */
    d = next_lane(4, 7, 8, false, false, false);
    expect(d.trigger && d.lane == 5, "two-way 8 lane step 4->5");

    /* 返程 / 泊车不接管 */
    d = next_lane(4, 7, 8, false, true, false);
    expect(!d.trigger, "on_return ignored");
    d = next_lane(4, 7, 8, false, false, true);
    expect(!d.trigger, "parking ignored");

    /* 目标越界：2 车道路段上的 7 号目标 → 忽略，不横移 */
    d = next_lane(1, 7, 2, false, false, false);
    expect(!d.trigger, "out-of-range goal ignored");

    if (g_fails) {
        std::fprintf(stderr, "%d route-goal test(s) failed\n", g_fails);
        return 1;
    }
    std::printf("behavior_route_goal: all tests passed\n");
    return 0;
}
