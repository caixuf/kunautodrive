/**
 * building.h — OSM 建筑几何（仿真核与前端共用的"单源真相"消费侧）
 *
 * 建筑数据来自 map.json 的 buildings[]（footprint 多边形 + height + xy + rotation），
 * 由 osm_to_map.py / build_osm_buildings.py 生成。此处把它收敛成 2D 有向包围盒
 * (OBB) 供两类用途：
 *   1) 碰撞：车辆 OBB vs 建筑 OBB（SAT，2D；高度不参与，与 collision.cpp 一致）
 *   2) 遮挡：ego→障碍物的视线线段是否被建筑足迹阻断（LOS）
 *
 * 刻意不依赖 road_network / esmini —— 只依赖 cJSON + 标准库，使 flowsim_node
 * （碰撞）与 perception_node（遮挡）都能直接包含，无需跨模块链接整条仿真核。
 */

#ifndef FLOWSIM_BUILDING_H
#define FLOWSIM_BUILDING_H

#include <cstddef>
#include <utility>
#include <vector>

namespace flowsim {

/** 建筑有向包围盒（由 footprint 投影到其主朝向后得到的最小包络 OBB）。 */
struct BuildingOBB {
    double x{0}, y{0};       /**< 质心（ENU 米，与道路同坐标系） */
    double len{0}, wid{0};   /**< OBB 长/宽（米），与 heading 对齐 */
    double heading{0};       /**< 主朝向（弧度，最长边方向） */
    double height{0};        /**< 高度（米，仅遮挡高度门控用） */
    /* footprint 原始多边形（世界坐标）：碰撞窄相用。OBB 对旋转/异形
     * footprint 严重高估（对角线建筑 OBB 面积可达 footprint 2-3 倍，
     * 陆家嘴隧道上方建筑 OBB 盖住路面 → ego 正常行驶被误判撞楼）。
     * 碰撞判定 = OBB SAT 粗筛 + 多边形精确窄相；poly 为空回退纯 OBB。 */
    std::vector<std::pair<double,double>> poly;
};

/**
 * 从 road_network JSON 字符串解析 buildings[] → BuildingOBB 列表。
 * JSON 形态：{ "edges":[...], "buildings":[ {id,footprint:[[x,y]...],x,y,rotation,height} ] }
 * footprint 为空时回退用 x/y/rotation/height 构造一个零尺寸盒（跳过碰撞）。
 */
void load_buildings(const char* road_network_json, std::vector<BuildingOBB>& out);

/**
 * 车辆 OBB 是否与建筑 OBB 相交（SAT，2D）。
 * @param vx,vy 车辆中心；vlen,vwid 尺寸；vh 航向
 */
bool obb_hits_building(double vx, double vy, double vlen, double vwid, double vh,
                       const BuildingOBB& b);

/**
 * 线段 a→b 是否被建筑足迹（OBB）阻断。用于视线遮挡（LOS）：若 ego→障碍物
 * 的连线穿过任一建筑，则障碍物被遮挡。2D（地面投影），高度门控由调用方决定。
 */
bool segment_intersects_building(double ax, double ay, double bx, double by,
                                 const BuildingOBB& b);

}  // namespace flowsim

#endif  // FLOWSIM_BUILDING_H
