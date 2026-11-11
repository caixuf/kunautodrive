#ifndef FLOWENGINE_OPEN_SPACE_ANGLE_UTILS_H
#define FLOWENGINE_OPEN_SPACE_ANGLE_UTILS_H

#include <cmath>

namespace flowengine {
namespace planning {

/**
 * 把角度规范到 [-π, π)。
 *
 * HybridAStar::Mod2Pi 与 ReedsShepp::Mod2Pi 原本各写一份逐字相同的实现；
 * 两套 Reeds-Shepp 公式对同一条路径的角度归一会得到不同结果的分叉风险
 * 不值得保留。保留两个类方法（内部调用点不变），公式收敛到此处。
 */
inline double NormalizeAnglePi(double angle) {
    double v = std::fmod(angle, 2.0 * M_PI);
    if (v < -M_PI) v += 2.0 * M_PI;
    else if (v >= M_PI) v -= 2.0 * M_PI;
    return v;
}

}  // namespace planning
}  // namespace flowengine

#endif  /* FLOWENGINE_OPEN_SPACE_ANGLE_UTILS_H */