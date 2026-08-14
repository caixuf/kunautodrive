/**
 * building.cpp — OSM 建筑几何实现（见 building.h）。
 *
 * 仅依赖 cJSON + 标准库，无 esmini / road_network 依赖。
 */

#include "building.h"

extern "C" {
#include <cjson/cJSON.h>
}

#include <cmath>
#include <algorithm>

namespace flowsim {

namespace {

/* 把 footprint 多边形投影到以 (cx,cy,heading) 为原点的局部帧，返回包络半长/半宽。 */
void footprint_extents(const cJSON* footprint, double cx, double cy, double heading,
                       double& half_len, double& half_wid) {
    half_len = 0.0;
    half_wid = 0.0;
    if (!cJSON_IsArray(footprint)) return;
    int n = cJSON_GetArraySize(footprint);
    if (n < 3) return;
    double ch = std::cos(heading), sh = std::sin(heading);
    double minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18;
    for (int i = 0; i < n; ++i) {
        const cJSON* p = cJSON_GetArrayItem(footprint, i);
        if (!cJSON_IsArray(p) || cJSON_GetArraySize(p) < 2) continue;
        const cJSON* jx = cJSON_GetArrayItem(p, 0);
        const cJSON* jy = cJSON_GetArrayItem(p, 1);
        if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) continue;
        double dx = jx->valuedouble - cx;
        double dy = jy->valuedouble - cy;
        // 旋转到建筑局部帧：x' 沿 heading，y' 垂直
        double lx = dx * ch + dy * sh;
        double ly = -dx * sh + dy * ch;
        minx = std::min(minx, lx); maxx = std::max(maxx, lx);
        miny = std::min(miny, ly); maxy = std::max(maxy, ly);
    }
    half_len = (maxx - minx) * 0.5;
    half_wid = (maxy - miny) * 0.5;
}

/* 单盒 4 角点 */
void obb_corners(double cx, double cy, double hl, double hw, double h, double out[4][2]) {
    double ch = std::cos(h), sh = std::sin(h);
    double local[4][2] = {{hl, hw}, {-hl, hw}, {-hl, -hw}, {hl, -hw}};
    for (int i = 0; i < 4; ++i) {
        out[i][0] = cx + local[i][0] * ch - local[i][1] * sh;
        out[i][1] = cy + local[i][0] * sh + local[i][1] * ch;
    }
}

void project(const double c[4][2], double ax, double ay, double& mn, double& mx) {
    mn = mx = c[0][0] * ax + c[0][1] * ay;
    for (int i = 1; i < 4; ++i) {
        double p = c[i][0] * ax + c[i][1] * ay;
        if (p < mn) mn = p;
        if (p > mx) mx = p;
    }
}

}  // namespace

void load_buildings(const char* road_network_json, std::vector<BuildingOBB>& out) {
    out.clear();
    if (!road_network_json) return;
    cJSON* root = cJSON_Parse(road_network_json);
    if (!root) return;
    cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "buildings");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        for (int i = 0; i < n; ++i) {
            cJSON* b = cJSON_GetArrayItem(arr, i);
            if (!cJSON_IsObject(b)) continue;
            cJSON* jx = cJSON_GetObjectItemCaseSensitive(b, "x");
            cJSON* jy = cJSON_GetObjectItemCaseSensitive(b, "y");
            cJSON* jrot = cJSON_GetObjectItemCaseSensitive(b, "rotation");
            cJSON* jh = cJSON_GetObjectItemCaseSensitive(b, "height");
            cJSON* jfp = cJSON_GetObjectItemCaseSensitive(b, "footprint");
            if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) continue;
            BuildingOBB obb;
            obb.x = jx->valuedouble;
            obb.y = jy->valuedouble;
            obb.heading = cJSON_IsNumber(jrot) ? jrot->valuedouble : 0.0;
            obb.height = cJSON_IsNumber(jh) ? jh->valuedouble : 12.0;
            if (cJSON_IsArray(jfp)) {
                footprint_extents(jfp, obb.x, obb.y, obb.heading, obb.len, obb.wid);
                obb.len *= 2.0;   // footprint_extents 返回半长/半宽
                obb.wid *= 2.0;
            }
            out.push_back(obb);
        }
    }
    cJSON_Delete(root);
}

bool obb_hits_building(double vx, double vy, double vlen, double vwid, double vh,
                       const BuildingOBB& b) {
    double c1[4][2], c2[4][2];
    obb_corners(vx, vy, vlen * 0.5, vwid * 0.5, vh, c1);
    obb_corners(b.x, b.y, b.len * 0.5, b.wid * 0.5, b.heading, c2);
    double axes[4][2] = {
        {std::cos(vh), std::sin(vh)},
        {-std::sin(vh), std::cos(vh)},
        {std::cos(b.heading), std::sin(b.heading)},
        {-std::sin(b.heading), std::cos(b.heading)},
    };
    for (int i = 0; i < 4; ++i) {
        double mn1, mx1, mn2, mx2;
        project(c1, axes[i][0], axes[i][1], mn1, mx1);
        project(c2, axes[i][0], axes[i][1], mn2, mx2);
        if (mx1 < mn2 || mx2 < mn1) return false;  // 任一轴分离 → 不相交
    }
    return true;
}

bool segment_intersects_building(double ax, double ay, double bx, double by,
                                 const BuildingOBB& b) {
    // 平移+旋转到建筑局部帧（中心为原点，x 沿 heading）
    double ch = std::cos(b.heading), sh = std::sin(b.heading);
    auto to_local = [&](double x, double y, double& lx, double& ly) {
        double dx = x - b.x, dy = y - b.y;
        lx = dx * ch + dy * sh;
        ly = -dx * sh + dy * ch;
    };
    double axl, ayl, bxl, byl;
    to_local(ax, ay, axl, ayl);
    to_local(bx, by, bxl, byl);

    // Liang-Barsky 线段裁剪到 [-hl,hl]×[-hw,hw] 盒；若线段有任何部分落在盒内/边上 → 相交
    double hl = b.len * 0.5, hw = b.wid * 0.5;
    double t0 = 0.0, t1 = 1.0;
    double dx = bxl - axl, dy = byl - ayl;
    int edges = 4;
    double p[4] = {-dx, dx, -dy, dy};
    double q[4] = {axl - (-hl), hl - axl, ayl - (-hw), hw - ayl};  // p<0 侧边界值
    // 标准 Liang-Barsky：p[k],q[k] 对应四边界 x>=-hl, x<=hl, y>=-hw, y<=hw
    for (int k = 0; k < edges; ++k) {
        if (p[k] == 0.0) {
            if (q[k] < 0.0) return false;  // 平行且在盒外
        } else {
            double r = q[k] / p[k];
            if (p[k] < 0.0) { if (r > t1) return false; if (r > t0) t0 = r; }
            else            { if (r < t0) return false; if (r < t1) t1 = r; }
        }
    }
    return t0 <= t1;  // 线段与盒存在重叠区间
}

}  // namespace flowsim
