/**
 * scene_events.cpp — 场景事件调度实现
 *
 * 红绿灯相位推进：基于仿真时间 fmod 计算当前相位。
 *   T = green + yellow + red
 *   tp = fmod(sim_time + offset, T)
 *   tp < green → Green
 *   tp < green+yellow → Yellow
 *   else → Red
 *
 * ETC 闸门：根据 ego 距离切换状态，抬杆进度线性插值。
 *
 * NPC 响应：前方有红灯/关闭闸门时，按制动距离减速到 0。
 */

#include "scene_events.h"
#include "npc_ai.h"
#include "physics.h"
#include "road_network.h"
#include "route.h"
#include "lane_frenet.h"   /* C-2: 共享车道中心横向偏移公式 */
#include "logger.h"
#include "scenario_loader.h"

#include <cmath>
#include <algorithm>

/* 仿真步长唯一事实源（60Hz ≈ 0.0167s）。本文件历史上自带一份 0.05 的定义，
 * 导致红绿灯 phase_timer 排水速度与实际帧率差 3 倍、编排 "hold red 10s" 实际只有 3.33s。 */
#include "flowsim_time.h"

namespace flowsim {
namespace {

constexpr int kChoreoBeatCapacity = SCENARIO_MAX_CHOREO_BEATS;
int g_last_triggered[kChoreoBeatCapacity];
bool g_choreo_init_done = false;

}  // namespace

void reset_choreography_state() {
    for (int& beat : g_last_triggered) beat = -1;
    g_choreo_init_done = false;
}

static bool eval_tl_phase_cycle(double sim_time_s, double green_s, double yellow_s,
                                double red_s, double offset,
                                int& state_out, double& timer_out) {
    if (green_s <= 0 && yellow_s <= 0 && red_s <= 0) return false;
    double T = green_s + yellow_s + red_s;
    if (T <= 0) return false;

    double tp = std::fmod(sim_time_s + offset, T);
    if (tp < 0) tp += T;

    const double flashing_s = std::min(TL_FLASHING_GREEN_SECONDS, green_s);
    const double steady_green_s = green_s - flashing_s;
    if (tp < steady_green_s) {
        state_out = (int)TLPhase::Green;
        timer_out = steady_green_s - tp;
    } else if (tp < green_s) {
        state_out = (int)TLPhase::FlashingGreen;
        timer_out = green_s - tp;
    } else if (tp < green_s + yellow_s) {
        state_out = (int)TLPhase::Yellow;
        timer_out = green_s + yellow_s - tp;
    } else {
        state_out = (int)TLPhase::Red;
        timer_out = T - tp;
    }
    return true;
}

void tick_traffic_lights(EntityPool& pool, double sim_time_s) {
    for (int i = 0; i < pool.size(); ++i) {
        Entity& tl = pool[i];
        if (!tl.active || tl.type != EntityType::TrafficLight) continue;

        /* choreography beat 相位覆盖锁定：phase_override_frames > 0 时不重算，
         * 仅递减 phase_timer 和计数器。让编舞 beat 设定的红/黄/绿灯持续一段时间
         * 而非 1 帧后被 fmod 时序重算覆盖。 */
        if (tl.phase_override_frames > 0) {
            tl.phase_override_frames--;
            tl.phase_timer -= FLOWSIM_DT_SEC;
            if (tl.phase_timer < 0.0) tl.phase_timer = 0.0;
            tl.left_phase_timer -= FLOWSIM_DT_SEC;
            if (tl.left_phase_timer < 0.0) tl.left_phase_timer = 0.0;
            continue;
        }

        // 相位时长复用 throttle/brake/steer 字段
        eval_tl_phase_cycle(sim_time_s, tl.throttle, tl.brake, tl.steer,
                            tl.target_vx, tl.phase_state, tl.phase_timer);
        if (tl.has_left_arrow) {
            if (!eval_tl_phase_cycle(sim_time_s, tl.left_green_s, tl.left_yellow_s,
                                     tl.left_red_s, tl.left_phase_offset_s,
                                     tl.left_phase_state, tl.left_phase_timer)) {
                tl.left_phase_state = tl.phase_state;
                tl.left_phase_timer = tl.phase_timer;
            }
        }
    }

    // 单路口正交方向多信号灯相位互斥校验（放行对向同步绿灯，拦截交叉方向同时绿灯）
    for (int i = 0; i < pool.size(); ++i) {
        const Entity& tl_i = pool[i];
        if (!tl_i.active || tl_i.type != EntityType::TrafficLight) continue;
        if (tl_i.phase_state != (int)TLPhase::Green &&
            tl_i.phase_state != (int)TLPhase::FlashingGreen) continue;
        for (int j = i + 1; j < pool.size(); ++j) {
            const Entity& tl_j = pool[j];
            if (!tl_j.active || tl_j.type != EntityType::TrafficLight) continue;
            if (tl_j.phase_state != (int)TLPhase::Green &&
                tl_j.phase_state != (int)TLPhase::FlashingGreen) continue;
            const double dist = std::hypot(tl_i.signal_stop_x - tl_j.signal_stop_x,
                                           tl_i.signal_stop_y - tl_j.signal_stop_y);
            if (dist <= 45.0 &&
                std::fabs(std::cos(tl_i.approach_heading - tl_j.approach_heading)) < 0.5) {
                LOG_WARN("flowsim",
                         "tick_traffic_lights: crossing traffic lights at same junction "
                         "(x=%.2f, y=%.2f) both Green - phase mutex violated",
                         tl_i.signal_stop_x, tl_i.signal_stop_y);
            }
        }
    }
}

void tick_etc_gates(EntityPool& pool, const Entity& ego, double dt) {
    for (int i = 0; i < pool.size(); ++i) {
        Entity& gate = pool[i];
        if (!gate.active || gate.type != EntityType::ETCGate) continue;

        /* 沿闸门朝向投影纵向/横向/高程距离：支持斜坡匝道、地面停车场入口等任意朝向与分层高程。
         * 对 ego 与所有活跃 NPC 车辆生效，避免 NPC 在闸门前永久死锁并堵塞后方车流。 */
        const double ch = std::cos(gate.heading);
        const double sh = std::sin(gate.heading);
        bool any_approaching = false;

        auto check_vehicle = [&](const Entity& v, double lat_limit) {
            const double dx = gate.x - v.x;
            const double dy = gate.y - v.y;
            const double dist = dx * ch + dy * sh;
            const double lat = -dx * sh + dy * ch;
            const double dz = std::fabs(v.z - gate.z);
            if (dz < 3.5 && std::fabs(lat) < lat_limit && dist > -4.0 && dist <= 35.0) {
                any_approaching = true;
            }
        };

        check_vehicle(ego, 8.0);
        if (!any_approaching) {
            for (int j = 0; j < pool.size(); ++j) {
                const Entity& npc = pool[j];
                if (!npc.active || !npc.is_npc_vehicle()) continue;
                check_vehicle(npc, 4.5);
                if (any_approaching) break;
            }
        }

        /* ETC/道闸状态用 phase_state：0=关闭 1=抬杆中 2=全开。
         * 车辆接近 35m 内开始抬杆（约 0.55s 完全升起），全开前保持物理障碍判定。 */
        if (any_approaching) {
            gate.phase_timer = std::min(1.0, gate.phase_timer + dt * 1.8);
            gate.phase_state = (gate.phase_timer >= 0.85) ? 2 : 1;
        } else {
            gate.phase_state = 0;
            gate.phase_timer = std::max(0.0, gate.phase_timer - dt * 1.0);
        }
    }
}

void check_npc_scene_events(EntityPool& pool, double look_ahead,
                             const NpcAiConfig& cfg) {
    for (int i = 0; i < pool.size(); ++i) {
        Entity& npc = pool[i];
        if (!npc.active || !npc.is_npc_vehicle()) continue;

        // 找前方最近的红灯/关闭闸门
        double nearest_block_s = 1e9;
        bool  should_stop = false;

        const double npc_ch = std::cos(npc.heading);
        const double npc_sh = std::sin(npc.heading);

        for (int j = 0; j < pool.size(); ++j) {
            const Entity& ev = pool[j];
            if (!ev.active) continue;
            if (ev.type != EntityType::TrafficLight && ev.type != EntityType::ETCGate) continue;
            if (std::fabs(ev.z - npc.z) > 2.5) continue;

            const bool is_tl = (ev.type == EntityType::TrafficLight);
            const double ev_x = (is_tl && (ev.signal_stop_x != 0.0 || ev.signal_stop_y != 0.0))
                                    ? ev.signal_stop_x : ev.x;
            const double ev_y = (is_tl && (ev.signal_stop_x != 0.0 || ev.signal_stop_y != 0.0))
                                    ? ev.signal_stop_y : ev.y;
            const double d_x = ev_x - npc.x;
            const double d_y = ev_y - npc.y;
            const double along = d_x * npc_ch + d_y * npc_sh;
            const double lat = std::fabs(-d_x * npc_sh + d_y * npc_ch);
            if (along <= 0.0 || along > look_ahead) continue;

            // 红灯/黄灯：按朝向投影匹配本方向来车（支持 E/W/N/S 任意四向路口与独立左转箭头）
            if (is_tl) {
                if (std::cos(ev.approach_heading - npc.heading) <= 0.5) continue;
                if (lat > 7.5) continue;
                const int eff_phase = (ev.has_left_arrow && npc.turn_intent == 1)
                                          ? ev.left_phase_state : ev.phase_state;
                if (eff_phase == (int)TLPhase::Red ||
                    eff_phase == (int)TLPhase::Yellow) {
                    if (along < nearest_block_s) {
                        nearest_block_s = along;
                        should_stop = true;
                    }
                }
            } else if (ev.type == EntityType::ETCGate) {
                // 未全开的道闸/ETC 闸门：NPC 进入 26m 传感区后停车等待抬杆，避免在 60m 外提前死锁
                if (lat <= 8.0 && ev.phase_state != 2 && along < 26.0 && along < nearest_block_s) {
                    nearest_block_s = along;
                    should_stop = true;
                }
            }
        }

        if (should_stop) {
            double brake_dist = npc.speed * npc.speed / (2.0 * npc.max_brake);
            if (nearest_block_s < brake_dist + 5.0) {
                NpcTransitionRequest req;
                req.event = NpcEvent::TL_Red;
                npc_request_state(npc, req, cfg);
            }
        } else if (npc.state == NpcState::StopForTL) {
            // 前方无红灯/黄灯 → 恢复巡航
            NpcTransitionRequest req;
            req.event = NpcEvent::TL_Green;
            npc_request_state(npc, req, cfg);
        }
    }
}

/* ── 编舞循环：每 loop_period_s 重置 actor 到 ego 附近 ──
 *
 * 复用 scene_events.cpp:44 的 fmod(sim_time+offset, T) 循环范式。
 * 用 last_triggered_loop[] 跟踪每个 beat 在当前 loop 周期内是否已触发，
 * 避免每 tick 重复触发同一个 beat。
 *
 * 红绿灯 beat：actor="tl" 时直接设置所有红绿灯的 phase_state。
 */
void tick_choreography(EntityPool& pool, const Entity& ego,
                       double sim_time_s, double dt,
                       const Choreography* choreo,
                       FlowRoadNetwork* roads,
                       const Route* route,
                       uint32_t cycle) {
    (void)dt;
    if (!choreo || !choreo->enabled || choreo->beat_count <= 0) return;
    if (choreo->loop_period_s <= 0.0) return;

    double T = choreo->loop_period_s;
    double phase = std::fmod(sim_time_s, T);
    if (phase < 0.0) phase += T;
    int current_loop = (int)(sim_time_s / T);

    /* 进程级缓存，跟踪每个 beat 上次触发的 loop 编号 */
    if (!g_choreo_init_done) {
        for (int& v : g_last_triggered) v = -1;
        g_choreo_init_done = true;
    }

    for (int i = 0; i < choreo->beat_count; ++i) {
        if (g_last_triggered[i] >= current_loop) continue;  /* 本轮已触发 */

        const ChoreoBeat* b = &choreo->beats[i];
        if (phase < b->t) continue;  /* 还没到节拍时间 */

        g_last_triggered[i] = current_loop;

        /* ── 红绿灯 beat ── */
        if (strcmp(b->actor, "tl") == 0) {
            int new_phase = (int)TLPhase::Green;  /* 默认 */
            double override_duration = 0.0;
            if (b->phase[0]) {
                if (strcmp(b->phase, "red") == 0) {
                    new_phase = (int)TLPhase::Red;
                    override_duration = 10.0;  /* 锁定 10s */
                } else if (strcmp(b->phase, "yellow") == 0) {
                    new_phase = (int)TLPhase::Yellow;
                    override_duration = 3.0;
                } else if (strcmp(b->phase, "green") == 0) {
                    new_phase = (int)TLPhase::Green;
                    override_duration = 10.0;
                }
            }
            for (int j = 0; j < pool.size(); ++j) {
                Entity& tl = pool[j];
                if (!tl.active || tl.type != EntityType::TrafficLight) continue;
                tl.phase_state = new_phase;
                tl.phase_timer = override_duration;
                tl.left_phase_state = new_phase;
                tl.left_phase_timer = override_duration;
                /* 锁定帧数 = duration / dt，让 tick_traffic_lights 跳过重算 */
                tl.phase_override_frames = (int)(override_duration / FLOWSIM_DT_SEC + 0.5);
            }
            continue;
        }

        /* ── actor beat：解析 actor id ── */
        int actor_id = atoi(b->actor);
        if (actor_id <= 0 && b->actor[0] != '0') continue;

        /* 查找 pool 中 actor_id 匹配的实体（按场景业务 id 匹配，e.id 是 pool 索引） */
        Entity* target = nullptr;
        for (int j = 0; j < pool.size(); ++j) {
            Entity& e = pool[j];
            /* ego 的 scenario_id 也是 0；跳过 ego，避免 beat(actor=0) 误改 ego。 */
            if (e.active && e.type != EntityType::Ego && e.scenario_id == actor_id) {
                target = &e; break;
            }
        }
        if (!target) continue;

        /* ── 动作类型判断：brake/cross 为"原地指令"，不重置位置 ── */
        bool is_brake = (strcmp(b->act, "brake") == 0);
        bool is_cross = (strcmp(b->act, "cross") == 0);
        /* cutin 是"连续横向机动"指令，不是传送：旧实现把 cutin 也当 teleport，
         * 无 ds/dl 的 beat 会直接把邻道车瞬移到 ego 车道（ego.y+0）→ 当场碰撞，
         * 且 cutin PID 根本没机会执行。与 brake/cross 一样只下指令、不重置位置。 */
        bool is_cutin = (strcmp(b->act, "cutin") == 0);
        bool teleport = !is_brake && !is_cross && !is_cutin;

        if (teleport) {
        /* ── NPC 变道防御性禁用 ──
         *
         * 用户需求：演示场景中 NPC 各守其道不变道，仅 ego 变道超车。
         * 即使场景文件写了 dl!=0 的 beat（如 dl=-3.5 横向瞬移到隔壁车道），
         * 也强制 dl=0，让 NPC 只在纵向被传送（重置到 ego 前方），不跨车道。
         *
         * 这从根上杜绝"choreography beat 让 NPC 变道"的行为，
         * 配合 MOBIL #if 0 禁用，NPC 横向控制权完全归零，仅按初始 offset 直行。
         *
         * 若需要 NPC 编导变道（如 cutin 法规场景），在 choreography 块里加
         * "allow_npc_lane_change": true —— 该开关已实现，见上方 effective_dl
         * 的条件判断。默认 false 保证既有场景零行为变化。 */
        double effective_dl = b->dl;
        if (target->is_vehicle() && target->type != EntityType::Ego) {
            /* NPC 横控默认归零（见下方注释），仅当场景显式开启
             * allow_npc_lane_change 时才让 beat 的 dl 真正驱动横向 ——
             * 这是 cut-in / cut-out 等法规场景的前提。 */
            if (!choreo->allow_npc_lane_change) effective_dl = 0.0;
        }

        /* ── 重置到 ego 前方 ──
         * dl 语义：相对 ego.y 的横向偏移。负值 = 向右（同向更外侧车道），
         * 正值 = 向左（靠近中心线/对向）。cutin 场景 NPC 应从右车道（dl<0）
         * 切入 ego 车道，而非从对向车道（dl>0）切入。
         *
         * 路面 clamp：ego 变道后 ego.y 可能已不在右车道（如 -5.25），
         * dl=-3.5 会把 NPC 放到 -8.75（路外）。clamp 到同向路面范围
         * [-same_dir_half, -0.3] 防止 NPC 被传送到路外或对向。 */
        double new_x = ego.x + b->ds;
        double new_y = ego.y + effective_dl;
        if (roads && roads->loaded()) {
            FrenetPos fp_ego;
            if (roads->world_to_frenet(ego.x, ego.y, fp_ego)) {
                int total = roads->drivable_lane_count(fp_ego.road_id, fp_ego.s);
                if (total >= 2) {
                    int same_dir = total / 2;
                    if (same_dir < 1) same_dir = total;
                    double same_dir_half = same_dir * 3.5;
                    if (ego.y < 0.0) {
                        if (new_y < -same_dir_half + 0.5) new_y = -same_dir_half + 0.5;
                        if (new_y > -0.3) new_y = -0.3;
                    } else {
                        if (new_y >  same_dir_half - 0.5) new_y =  same_dir_half - 0.5;
                        if (new_y <  0.3) new_y =  0.3;
                    }
                }
            }
        }
        target->x = new_x;
        target->y = new_y;
        target->vx = b->vx;
        target->vy = 0.0;
        target->speed = std::fabs(b->vx);
        target->heading = 0.0;  /* 沿 +x 直行 */
        /* 标记本次显式传送，供 temporal invariant 跳过 Δpos 检查。
         * choreography beat 是场景循环机制（非 bug），传送后下一帧
         * invariant 会跳过 pos 检查，避免误报瞬移。 */
        target->last_teleport_cycle = cycle;

        /* ── 同步 Frenet 状态 ──
         * choreography 传送 NPC 到新世界坐标后，必须同步其 Frenet 状态
         * (offset/route_s/road_pos)，否则下一帧 step_npc_vehicle 会用旧
         * Frenet 状态把 NPC 拉回原位（road_pos.world() 覆盖 x/y）。
         * 用 world_to_frenet 重新计算 (road_id, s, offset)，然后 reinit
         * road_pos 和 route_s。 */
        if (roads && roads->loaded()) {
            FrenetPos fp;
            if (roads->world_to_frenet(new_x, new_y, fp)) {
                target->road_id = fp.road_id;
                target->s       = fp.s;
                /* fp.offset 是车道内偏移，需换算成相对参考线的横向偏移。
                 * C-2: 用 lane_frenet.h::offset_from_lane_internal 替换手写公式。 */
                target->offset = offset_from_lane_internal(*roads, fp.road_id,
                                                            fp.lane_id, fp.s, fp.offset);
                /* 非 cutin 时 target_offset = offset（保持当前车道） */
                /* cutin 的 target_offset 在下方动作块设置 */
                /* P4: 用 relocate 而非 init，避免 RM_DeletePosition 破坏 esmini
                 * handle 数组导致其他 NPC handle 指向错误位置。 */
                if (target->road_pos.ok()) {
                    target->road_pos.relocate(*roads, fp.road_id, 0, fp.s, target->offset);
                }
                /* 同步 route_s */
                if (route && route->ok()) {
                    int ei = route->index_of(fp.road_id);
                    if (ei >= 0) {
                        target->route_s = route->to_route_s(ei, fp.s);
                    }
                }
            }
        }
        } /* end teleport block */

        /* ── 动作：统一状态转移 ── */
        NpcTransitionRequest req;
        if (is_brake) {
            /* brake：原地设置目标速度，让 NPC 自然减速/加速到 b->vx，不传送 */
            req.event = NpcEvent::ScriptSet;
            req.target_state = NpcState::Cruise;
            req.target_vx = b->vx;
        } else if (is_cross) {
            /* cross：让行人开始横穿（vy = b->vx，正值=向左横穿） */
            if (target->type == EntityType::Pedestrian) {
                target->vy = b->vx;        /* b->vx 字段复用为横穿速度 (m/s) */
                target->ped_parked = 0;    /* 取消路边等待状态 */
                target->ped_wait_timer = 0.0;
                /* vx=0 保持不纵向移动，横向速度由 step_npc_pedestrian 积分 */
                target->vx = 0.0;
                LOG_INFO("flowsim", "choreo cross t=%.1f ped_id=%d pos=(%.1f,%.1f) vy=%.1f (loop=%d)",
                         b->t, actor_id, target->x, target->y, target->vy, current_loop);
            }
            continue; /* cross 不走 npc_request_state */
        } else if (b->act[0]) {
            if (strcmp(b->act, "overtake") == 0) {
                req.event = NpcEvent::ChoreoOvertake;
                req.target_vx = b->vx;
                req.target_offset = target->offset;
            } else if (strcmp(b->act, "cutin") == 0) {
                req.event = NpcEvent::ChoreoCutIn;
                /* target_offset 是**道路参考线**系横向偏移，不是世界 y。
                 * 直道（参考线沿 x 轴）两者相等，所以旧写法在 straight_road
                 * 上"碰巧"能跑；弯道/旋转的 OSM 路上 ego.y 与 ref offset 差一个
                 * 参考线世界偏移，会把 NPC 送到路外。 */
                double cutin_target = ego.y;
                if (roads && roads->loaded()) {
                    FrenetPos fp_ego;
                    if (roads->world_to_frenet(ego.x, ego.y, fp_ego, ego.z)) {
                        cutin_target = offset_from_lane_internal(
                            *roads, fp_ego.road_id, fp_ego.lane_id,
                            fp_ego.s, fp_ego.offset);
                    }
                }
                if (target->route_dir > 0 && cutin_target > -0.3) cutin_target = -0.3;
                if (target->route_dir < 0 && cutin_target <  0.3) cutin_target =  0.3;
                req.target_offset = cutin_target;
                req.target_vx = b->vx;
            }
        } else {
            /* 无 act：仅重置位置速度 */
            req.event = NpcEvent::ChoreoOvertake;
            req.target_vx = b->vx;
            req.target_offset = target->offset;
        }
        /* 统一入口转移 NPC 状态 */
        {
            NpcAiConfig choreo_cfg;  /* 使用默认配置 */
            npc_request_state(*target, req, choreo_cfg);
        }

        if (is_brake) {
            LOG_INFO("flowsim", "choreo brake t=%.1f actor=%d pos=(%.1f,%.1f) target_vx=%.1f cur_v=%.1f (loop=%d)",
                     b->t, actor_id, target->x, target->y, b->vx, target->speed, current_loop);
        } else {
            LOG_INFO("flowsim", "choreo beat t=%.1f actor=%d pos=(%.1f,%.1f) offset=%.2f vx=%.1f act='%s' (loop=%d)",
                     b->t, actor_id, target->x, target->y, target->offset, b->vx, b->act, current_loop);
        }
    }
}

}  // namespace flowsim
