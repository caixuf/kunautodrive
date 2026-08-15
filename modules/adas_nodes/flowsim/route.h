/**
 * route.h — 中央有序道路 route（NPC 车道跟随的骨架）
 *
 * 背景：NPC 原本用 step_bicycle(steer=0) 在世界系直线积分，道路一拐弯车就
 * 全飞出路网。本类把 esmini 各 road 按端点连续性 + 航向连续性链成 **一条有序
 * 主 route**，NPC 沿 route 的累计 s 推进；过 road 边界即进入 route 的下一段
 * （中央调度，不依赖 esmini 的过路口 API —— 封装里也没有 RM_PositionMoveForward）。
 *
 * 用法：
 *   Route route;
 *   route.build(roads);                       // 场景加载后构建一次
 *   int rid; double sl; int idx;
 *   route.locate(route_s, rid, sl, idx);      // 累计 s → (road_id, esmini s（含 s0 修剪偏移）, 段号)
 *   roads.frenet_to_world(rid, 0, sl, offset, wp);   // 再反算世界坐标
 */

#ifndef FLOWSIM_ROUTE_H
#define FLOWSIM_ROUTE_H

#include <utility>
#include <vector>

namespace flowsim {

class FlowRoadNetwork;

/** 参考路径采样点：供 control_node Stanley 横向控制消费。
 *  - (x, y, h)   世界坐标 + 切线航向 rad
 *  - kappa      曲率 1/m（数值微分估计，0=直线，正=左弯，负=右弯）
 *  - route_s    所属 route 累计 s（用于触发分支切换等） */
struct RefPathPoint {
    double x{0.0};
    double y{0.0};
    double h{0.0};
    double kappa{0.0};
    double route_s{0.0};
};

/** route 的一段（= 一条 esmini road），带累计 s 起点。 */
struct RouteSeg {
    int    road_id{0};
    double length{0.0};
    double s_start{0.0};   /**< 该段在 route 坐标系里的累计 s 起点 */

    /* 路口盒缺口合成连接段（2026-08-15 OSM 修复）：真实道路在路口盒边界断开
     * （OSM way 在交叉口节点处分割，盒内无几何），chain 相邻段端点可差 10-25m。
     * 急转角（fillet 半径 <10m）时两侧真实段各退 R·tan(dh/2) 修剪，缺口用
     * 端点切线约束的三次 Hermite 折线桥接（切向量长取圆弧逼近 (4/3)R·tan(dh/4)，
     * 保证 max|kappa| ≤ 1/R 可驾驶）；sample_ahead/project 直接插值折线，
     * 不走 esmini 查询。无几何跳变 → ref_path 连续 → 路口转向可跟踪。 */
    bool                      is_virtual{false};
    std::vector<RefPathPoint> pts;         /**< 虚拟段折线（is_virtual 时有效） */

    /** 真实段在 esmini road 上的起始 s（路口 fillet 修剪段首时 >0；
     *  esmini 坐标 = s0 + route 局部 s；段末 = s0 + length，可被下一 junction
     *  的 fillet 修短）。 */
    double                    s0{0.0};
};

/**
 * 中央有序 route。单线程构建/查询（与 FlowRoadNetwork 一致）。
 */
class Route {
public:
    /**
     * 从路网构建主 route。
     * 算法：取「无前驱」road 作起点（其 start 端点不与任何 road 的 end 重合），
     * 之后每步在未用 road 里挑 start 端点最接近当前 end、且航向最连续者为后继，
     * 长度更长者优先做 tiebreak（避免误选短存根/急弯匝道）。
     * @param tol 端点重合判定容差 (m)
     * @return 至少链出一段返回 true
     */
    bool build(FlowRoadNetwork& roads, double tol = 4.0);

    /**
     * 按显式 road_id 有序链构建 route（road_chain 契约）。
     * 用于 A* 输出的车道链去重 road 后 → Route，替换/补充自动链式 build()：
     * road_chain 已由路由层保证拓扑连续，无需再按端点几何猜测后继。
     * @param roads    路网（查 road 长度）
     * @param road_ids 有序 road 数字 id 数组（esmini 数值 id，与 map edge id 一致）
     * @param count    数组长度
     * @return 至少链出一段返回 true（图外 road 处截断，保留已链部分）
     */
    bool build_from_chain(FlowRoadNetwork& roads, const int* road_ids, int count);

    bool   ok() const { return !segs_.empty(); }
    int    count() const { return static_cast<int>(segs_.size()); }
    double total_length() const { return total_; }
    const RouteSeg& seg(int idx) const { return segs_[idx]; }

    /** road_id → route 段号；不在 route 上返回 -1。 */
    int index_of(int road_id) const;

    /** route 累计 s → (road_id, 段内 s_local, 段号)。route_s 夹到 [0,total]。 */
    void locate(double route_s, int& road_id, double& s_local, int& route_idx) const;

    /** (段号, 段内 s) → route 累计 s。参数非法返回 0。 */
    double to_route_s(int route_idx, double s_local) const;

    /**
     * route 累计 s → 世界位姿 (x, y, h)。真实段走 esmini frenet→world；
     * 虚拟段（路口盒连接段）沿合成折线插值。sample_ahead / project 的统一入口。
     * @return false = 查询失败（路网未加载/虚拟段为空）
     */
    bool sample_pose(FlowRoadNetwork& roads, double route_s,
                     double& x, double& y, double& h) const;

    /**
     * 世界点 (x,y) → route 最近点投影，返回 route 累计 s。
     * 为 OSM 平行对向车道/路口区域设计：nearest-road 投影（world_to_frenet）
     * 会在平行车道洞/路口连接段之间跳变（陆家嘴双洞隧道实测 ego 被拽到
     * 对向洞、route_s 跳变 1000m+），而 route 是唯一权威行进路径——把 ego
     * 投到 route 几何上并用 hint 限定搜索窗口（跟踪单调性），投影即稳定。
     *
     * @param roads        路网（frenet→world 用）
     * @param x, y         世界坐标
     * @param hint_route_s 窗口中心（上帧结果）；<0 时全 route 扫描（初始化一次）
     * @param window       搜索窗口半宽（m），hint>=0 时搜 [hint-window, hint+window]
     * @return 最近点 route_s；路网全失败返回 -1
     */
    double project(FlowRoadNetwork& roads, double x, double y,
                   double hint_route_s, double window = 150.0) const;

    /**
     * 在 route 上从 route_s_start 起向前采样 N 个参考点，跨段拼接。
     * 用于 ego route-following：control_node Stanley 横向控制消费。
     * 每个点用 frenet_to_world 反算世界坐标 + 航向；曲率 kappa 由
     * 相邻点弦角 / 弦长数值微分估计（不需要解析几何，对 line/arc 通用）。
     *
     * @param roads        路网（frenet→world 用）
     * @param route_s_start 起点 route 累计 s（夹到 [0, total]）
     * @param lookahead    前瞻总长 m（采样到 route_s_start + lookahead 或 route 终点）
     * @param step_m       采样间距 m（默认 5m）
     * @param out          输出采样点（清空后追加），点数 = ceil(lookahead/step_m)+1
     * @param reverse      false=沿 +route_s 采样（正常前进）；true=沿 -route_s
     *                     采样并把每点航向翻转 π（对向车道掉头后，ego 真实行进
     *                     方向是 route_s 递减方向）。输出点始终按「行进方向前方」
     *                     有序：out[0]=起点，后续点是行进方向前方，heading 已朝
     *                     实际行进方向，kappa 在行进坐标系下计算。
     * @return 实际采样点数（路网失败/空 route 时返回 0）
     */
    int sample_ahead(FlowRoadNetwork& roads, double route_s_start,
                     double lookahead, double step_m,
                     std::vector<RefPathPoint>& out,
                     bool reverse = false) const;

private:
    std::vector<RouteSeg>                segs_;
    double                              total_{0.0};
};

}  // namespace flowsim

#endif  // FLOWSIM_ROUTE_H
