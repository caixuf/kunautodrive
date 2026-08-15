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
                /* 保留原始 footprint（世界坐标）供碰撞窄相 */
                int np = cJSON_GetArraySize(jfp);
                obb.poly.reserve((size_t)np);
                for (int pi = 0; pi < np; ++pi) {
                    cJSON* pt = cJSON_GetArrayItem(jfp, pi);
                    if (!cJSON_IsArray(pt) || cJSON_GetArraySize(pt) < 2) continue;
                    cJSON* px = cJSON_GetArrayItem(pt, 0);
                    cJSON* py = cJSON_GetArrayItem(pt, 1);
                    if (cJSON_IsNumber(px) && cJSON_IsNumber(py))
                        obb.poly.emplace_back(px->valuedouble, py->valuedouble);
                }
                if (obb.poly.size() < 3) obb.poly.clear();
            }
            out.push_back(obb);
        }
    }
    cJSON_Delete(root);
}

/* ── 多边形窄相（footprint 精确） ── */

/* 射线法：点是否在简单多边形内（含边界容差由调用方保证）。 */
static bool point_in_poly(double px, double py,
                          const std::vector<std::pair<double,double>>& poly) {
    bool inside = false;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        double xi = poly[i].first,  yi = poly[i].second;
        double xj = poly[j].first,  yj = poly[j].second;
        if ((yi > py) != (yj > py)) {
            double xint = (xj - xi) * (py - yi) / (yj - yi) + xi;
            if (px < xint) inside = !inside;
        }
    }
    return inside;
}

/* 线段相交（含共线重叠的稳健取向测试）。 */
static bool seg_intersect(double ax, double ay, double bx, double by,
                          double cx, double cy, double dx, double dy) {
    auto cross = [](double ox, double oy, double px, double py, double qx, double qy) {
        return (px - ox) * (qy - oy) - (py - oy) * (qx - ox);
    };
    double d1 = cross(cx, cy, dx, dy, ax, ay);
    double d2 = cross(cx, cy, dx, dy, bx, by);
    double d3 = cross(ax, ay, bx, by, cx, cy);
    double d4 = cross(ax, ay, bx, by, dx, dy);
    if (((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0))) return true;
    return false;  /* 共线擦边不算碰撞（行驶贴线场景容差） */
}

/* 车辆 OBB vs 建筑 footprint 多边形：角点互含 + 边相交。 */
static bool obb_hits_poly(double vx, double vy, double vlen, double vwid, double vh,
                          const std::vector<std::pair<double,double>>& poly) {
    if (poly.size() < 3) return false;
    double c1[4][2];
    obb_corners(vx, vy, vlen * 0.5, vwid * 0.5, vh, c1);
    /* 1) 车角点落入多边形 */
    for (int i = 0; i < 4; ++i)
        if (point_in_poly(c1[i][0], c1[i][1], poly)) return true;
    /* 2) 多边形顶点落入车 OBB（车局部帧判定） */
    double ch = std::cos(vh), sh = std::sin(vh);
    double hl = vlen * 0.5, hw = vwid * 0.5;
    for (const auto& p : poly) {
        double ddx = p.first - vx, ddy = p.second - vy;
        double lx = ddx * ch + ddy * sh;
        double ly = -ddx * sh + ddy * ch;
        if (std::fabs(lx) <= hl && std::fabs(ly) <= hw) return true;
    }
    /* 3) 边-边相交 */
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        double ex0 = poly[j].first, ey0 = poly[j].second;
        double ex1 = poly[i].first, ey1 = poly[i].second;
        for (int k = 0; k < 4; ++k) {
            int k2 = (k + 1) % 4;
            if (seg_intersect(ex0, ey0, ex1, ey1,
                              c1[k][0], c1[k][1], c1[k2][0], c1[k2][1]))
                return true;
        }
    }
    return false;
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
    /* OBB 粗筛通过：有 footprint 多边形时做精确窄相（旋转/异形建筑的 OBB
     * 高估可达 2-3 倍——陆家嘴隧道上方建筑 OBB 盖住路面，纯 OBB 误判撞楼）。 */
    if (b.poly.size() >= 3)
        return obb_hits_poly(vx, vy, vlen, vwid, vh, b.poly);
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
