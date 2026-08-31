#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <random>
#include <memory>
#include <sstream>
#include <iostream>

namespace kun {

// ============================================================================
// 1. 细胞类型 (CellType): 生物计算原语
// ============================================================================
enum class CellType : uint8_t {
    // 【感知受体细胞 (Receptor / Sensory)】
    SENSE_RAW_INPUT_0 = 0,   // 输入通道 0 (量化: 价格 / 智驾: 目标物距离)
    SENSE_RAW_INPUT_1 = 1,   // 输入通道 1 (量化: 成交量 / 智驾: 相对速度)
    SENSE_RAW_INPUT_2 = 2,   // 输入通道 2 (量化: 盘口价差 / 智驾: 车道线偏离)
    SENSE_RAW_INPUT_3 = 3,   // 输入通道 3 (量化: 委托不平衡 / 智驾: TTC碰撞时距)
    
    // 【代谢运算细胞 (Metabolic Math Operators)】
    OP_EMA = 10,             // 指数平滑滤波 (具备衰减记忆)
    OP_DIFF = 11,            // 微分/变化率 (感知加速度与斜率)
    OP_INTEGRAL = 12,        // 积分累加 (能量积聚与趋势持久度)
    OP_SUM = 13,             // 突触信号线性叠加 (Input A + Input B)
    OP_SUB = 14,             // 突触信号差分对比 (Input A - Input B)
    OP_MULTIPLY = 15,        // 增益调节与调制 (Input A * Input B)
    OP_RATIO = 16,           // 相对比率 (Input A / (Input B + eps))
    OP_ABS = 17,             // 绝对值幅度 (|Input A|)
    
    // 【门控逻辑神经元 (Gating / Threshold Neurons)】
    GATE_THRESHOLD = 20,     // 阶跃阈值激活 (Input > param ? 1 : 0)
    GATE_HYSTERESIS = 21,    // 迟滞比较器 (Schmitt Trigger，防震荡高频抖动)
    GATE_AND = 22,           // 协同兴奋门 (Input A > 0 && Input B > 0)
    GATE_INHIBIT = 23,       // 抑制性突触 (Input A * (1.0 - Input B))
    
    // 【效应/动作细胞 (Effector / Action)】
    ACT_PRIMARY_POSITIVE = 30, // 正向激发动作 (量化: 买开仓 / 智驾: 变道加速)
    ACT_PRIMARY_NEGATIVE = 31, // 反向激发动作 (量化: 卖开仓 / 智驾: 减速避让)
    ACT_DEFENSIVE_RESET  = 32, // 防御性归零 (量化: 平仓清空 / 智驾: 保持车道居中)
    ACT_IMMUNE_BLOCK     = 33  // 免疫阻断刹车 (量化: 熔断锁定 / 智驾: AEB紧急制动)
};

inline const char* to_string(CellType t) {
    switch (t) {
        case CellType::SENSE_RAW_INPUT_0: return "Sense_Input0";
        case CellType::SENSE_RAW_INPUT_1: return "Sense_Input1";
        case CellType::SENSE_RAW_INPUT_2: return "Sense_Input2";
        case CellType::SENSE_RAW_INPUT_3: return "Sense_Input3";
        case CellType::OP_EMA: return "Op_EMA";
        case CellType::OP_DIFF: return "Op_Diff";
        case CellType::OP_INTEGRAL: return "Op_Integral";
        case CellType::OP_SUM: return "Op_Sum";
        case CellType::OP_SUB: return "Op_Sub";
        case CellType::OP_MULTIPLY: return "Op_Multiply";
        case CellType::OP_RATIO: return "Op_Ratio";
        case CellType::OP_ABS: return "Op_Abs";
        case CellType::GATE_THRESHOLD: return "Gate_Threshold";
        case CellType::GATE_HYSTERESIS: return "Gate_Hysteresis";
        case CellType::GATE_AND: return "Gate_And";
        case CellType::GATE_INHIBIT: return "Gate_Inhibit";
        case CellType::ACT_PRIMARY_POSITIVE: return "Act_PosAction";
        case CellType::ACT_PRIMARY_NEGATIVE: return "Act_NegAction";
        case CellType::ACT_DEFENSIVE_RESET: return "Act_DefReset";
        case CellType::ACT_IMMUNE_BLOCK: return "Act_ImmuneLock";
        default: return "Cell_Unknown";
    }
}

// ============================================================================
// 2. 突触连接 (Synapse): 细胞间传递与调控通路 (原生内嵌力学弹簧与放电粒子)
// ============================================================================
struct Synapse {
    uint16_t from_cell_id{0};  // 发射端细胞 ID (突触前膜)
    uint16_t to_cell_id{0};    // 接收端细胞 ID (突触后膜)
    uint8_t  to_port{0};       // 目标细胞端口号 (0=主输入, 1=辅助输入/门控端)
    double   weight{1.0};      // 传递强度 (可塑性突触权重)
    bool     is_active{true};  // 突触激活态

    // === 原生一等公民力学与全息可视化属性 ===
    float rest_length{60.0f};  // 弹簧物理静止长度
    float photon_pos{-1.0f};   // 神经电冲动放电光子位置 (0.0 -> 1.0)
};

// ============================================================================
// 3. 细胞实例 (Cell): 独立生命计算节点 (原生内嵌 3D 坐标、力学与生物发光)
// ============================================================================
struct Cell {
    uint16_t id{0};
    CellType type{CellType::OP_EMA};
    
    // 细胞参数 (如 EMA 衰减系数 alpha, 门控阈值等)
    double param1{0.1};
    double param2{0.0};
    
    // 内部生物状态 (记忆与积分)
    double state_val{0.0};
    double prev_input{0.0};
    bool   latch_state{false};
    
    // 运行时当前放电电位 (Output Voltage)
    double output_val{0.0};
    
    // 细胞活跃度计数与代谢生命期 (用于凋亡剪枝)
    uint32_t activation_count{0};
    uint64_t last_active_ts{0};

    // === 原生一等公民 3D 物理空间坐标与力学属性 ===
    float x{0.0f}, y{0.0f}, z{0.0f};       // 培养皿空间坐标
    float vx{0.0f}, vy{0.0f}, vz{0.0f};    // 速度矢量
    float fx{0.0f}, fy{0.0f}, fz{0.0f};    // 受到的合力 (兰纳-琼斯力场)
    float glow_charge{0.0f};               // 生物发光强度 (0.0 ~ 1.0, 脉冲放电后渐变衰减)
};

// ============================================================================
// 4. 多细胞有机体 (CellularOrganism): 细胞拓扑决策图 (DAG) + 力场物理引擎
// ============================================================================
class CellularOrganism {
public:
    uint64_t organism_id{0};
    uint32_t generation{0};
    std::string lineage_name;   // 物种族谱名 (如 "Apex-Predator-V3")
    
    std::vector<Cell> cells;
    std::vector<Synapse> synapses;
    
    // 适应度评估 (夏普比率、胜率、稳定性)
    double fitness_score{0.0};
    double total_pnl{0.0};
    double max_drawdown{0.0};
    uint32_t trade_count{0};
    
    // 预编译扁平执行序列 (真·零 GC，无哈希表，无堆分配)
    struct CompiledSynapse {
        size_t from_idx;
        size_t to_idx;
        uint8_t to_port;
        double weight;
    };
    std::vector<CompiledSynapse> compiled_synapses_;
    std::vector<size_t> execution_order_;
    mutable std::vector<double> flat_port_inputs_; // [cell_idx * 2 + port]
    bool is_compiled_{false};
    bool is_compiled() const { return is_compiled_; }

    CellularOrganism() = default;

    // 回合间状态重置: 清零动态膜电位与记忆, 保留基因组 (参数/拓扑/坐标) 不变。
    // 无此重置则 EMA/迟滞等记忆细胞的跨回合残留会污染适应度评估 (评估噪声 → 精英保留失效)。
    void reset_state() {
        for (auto& c : cells) {
            c.state_val = 0.0;
            c.prev_input = 0.0;
            c.latch_state = false;
            c.output_val = 0.0;
        }
        std::fill(flat_port_inputs_.begin(), flat_port_inputs_.end(), 0.0);
    }

    // 创建最简单细胞原生生物 (Archean Progenitor)
    static CellularOrganism create_seed_organism(uint64_t id = 1) {
        CellularOrganism org;
        org.organism_id = id;
        org.generation = 0;
        org.lineage_name = "Genesis-0";

        // 感知细胞: 0=主输入, 1=辅助输入
        org.cells.push_back({0, CellType::SENSE_RAW_INPUT_0, 1.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, -120.0f, -40.0f, 0.0f});
        org.cells.push_back({1, CellType::SENSE_RAW_INPUT_1, 1.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, -120.0f,  40.0f, 0.0f});
        
        // 代谢细胞: EMA 滤波
        org.cells.push_back({2, CellType::OP_EMA, 0.05, 0.0, 0.0, 0.0, false, 0.0, 0, 0, -40.0f, -30.0f, 0.0f}); // 慢线
        org.cells.push_back({3, CellType::OP_EMA, 0.20, 0.0, 0.0, 0.0, false, 0.0, 0, 0, -40.0f,  30.0f, 0.0f}); // 快线
        
        // 门控对比细胞: 快线减慢线差值 + 迟滞比较
        org.cells.push_back({4, CellType::OP_SUB, 0.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, 30.0f, 0.0f, 0.0f});
        org.cells.push_back({5, CellType::GATE_HYSTERESIS, 0.01, -0.01, 0.0, 0.0, false, 0.0, 0, 0, 80.0f, 0.0f, 0.0f});

        // 效应动作细胞
        org.cells.push_back({6, CellType::ACT_PRIMARY_POSITIVE, 0.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, 140.0f, -40.0f, 0.0f}); // 买/加速
        org.cells.push_back({7, CellType::ACT_PRIMARY_NEGATIVE, 0.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, 140.0f,  40.0f, 0.0f}); // 卖/制动
        org.cells.push_back({8, CellType::ACT_IMMUNE_BLOCK, 0.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, 140.0f, 100.0f, 0.0f});     // 免疫锁

        // 连接突触
        org.synapses.push_back({0, 2, 0, 1.0, true, 60.0f, -1.0f});
        org.synapses.push_back({0, 3, 0, 1.0, true, 60.0f, -1.0f});
        org.synapses.push_back({3, 4, 0, 1.0, true, 60.0f, -1.0f});
        org.synapses.push_back({2, 4, 1, 1.0, true, 60.0f, -1.0f});
        org.synapses.push_back({4, 5, 0, 1.0, true, 50.0f, -1.0f});
        org.synapses.push_back({5, 6, 0, 1.0, true, 60.0f, -1.0f});
        org.synapses.push_back({5, 7, 0, -1.0, true, 60.0f, -1.0f});

        org.compile();
        return org;
    }

    // 拓扑排序与扁平执行编译 (Flat Array Compilation: 消除所有运行期堆分配)
    bool compile() {
        std::unordered_map<uint16_t, size_t> id_to_idx;
        for (size_t i = 0; i < cells.size(); ++i) {
            id_to_idx[cells[i].id] = i;
        }

        std::unordered_map<uint16_t, int> in_degrees;
        std::unordered_map<uint16_t, std::vector<uint16_t>> adj;
        for (const auto& c : cells) in_degrees[c.id] = 0;

        compiled_synapses_.clear();
        for (const auto& syn : synapses) {
            if (!syn.is_active) continue;
            auto it_from = id_to_idx.find(syn.from_cell_id);
            auto it_to = id_to_idx.find(syn.to_cell_id);
            if (it_from != id_to_idx.end() && it_to != id_to_idx.end()) {
                adj[syn.from_cell_id].push_back(syn.to_cell_id);
                in_degrees[syn.to_cell_id]++;
                compiled_synapses_.push_back({it_from->second, it_to->second, syn.to_port, syn.weight});
            }
        }

        std::vector<uint16_t> queue;
        for (const auto& c : cells) {
            if (in_degrees[c.id] == 0) {
                queue.push_back(c.id);
            }
        }

        execution_order_.clear();
        size_t head = 0;
        while (head < queue.size()) {
            uint16_t u = queue[head++];
            execution_order_.push_back(id_to_idx[u]);

            for (uint16_t v : adj[u]) {
                in_degrees[v]--;
                if (in_degrees[v] == 0) {
                    queue.push_back(v);
                }
            }
        }

        // 环保护补齐
        if (execution_order_.size() < cells.size()) {
            std::unordered_set<size_t> visited(execution_order_.begin(), execution_order_.end());
            for (size_t i = 0; i < cells.size(); ++i) {
                if (visited.find(i) == visited.end()) {
                    execution_order_.push_back(i);
                }
            }
        }

        // 预分配扁平输入端口缓冲 (每个细胞 2 个端口)
        flat_port_inputs_.assign(cells.size() * 2, 0.0);
        is_compiled_ = true;
        return true;
    }

    // ========================================================================
    // 5. 严格 12-6 兰纳-琼斯势能与力场微物理引擎 (Lennard-Jones 12-6 Potential Engine)
    // V_LJ(r) = 4*epsilon * [ (sigma/r)^12 - (sigma/r)^6 ]
    // F_LJ(r) = -grad V = (24*epsilon / r^2) * [ 2*(sigma/r)^12 - (sigma/r)^6 ] * r_vec
    // ========================================================================
    void step_force_field_physics(float dt = 0.016f) {
        const float epsilon = 15.0f;       // 势阱深度 (Potential Well Depth)
        const float sigma = 35.0f;         // 零势平衡距离 (Collision Diameter)
        const float sigma6 = std::pow(sigma, 6.0f);
        const float sigma12 = sigma6 * sigma6;
        const float r_cut = 3.0f * sigma;  // 截断半径 (Cutoff Radius)
        const float k_spring = 0.05f;      // 突触结构弹簧系数
        const float damping = 0.85f;       // 黏性阻尼

        // 1. 重置合力并衰减发光电位
        for (auto& c : cells) {
            c.fx = 0.0f; c.fy = 0.0f; c.fz = 0.0f;
            c.glow_charge *= 0.92f;
        }

        // 2. 严格 12-6 兰纳-琼斯多体非键结势能力场 (3D 近斥中吸远无)
        for (size_t i = 0; i < cells.size(); ++i) {
            for (size_t j = i + 1; j < cells.size(); ++j) {
                float dx = cells[j].x - cells[i].x;
                float dy = cells[j].y - cells[i].y;
                float dz = cells[j].z - cells[i].z;
                float dist_sq = dx * dx + dy * dy + dz * dz + 1e-4f;
                float dist = std::sqrt(dist_sq);

                if (dist < r_cut && dist > 1.0f) {
                    float r2 = dist_sq;
                    float r6 = r2 * r2 * r2;
                    float r12 = r6 * r6;

                    // 12-6 势解析导数力标量: f_mag > 0 为斥力, f_mag < 0 为吸引力
                    float sr6 = sigma6 / r6;
                    float sr12 = sigma12 / r12;
                    float f_mag = (24.0f * epsilon / r2) * (2.0f * sr12 - sr6);

                    // 避免极端近距离数值发散溢出 (Force Cap)
                    f_mag = std::clamp(f_mag, -50.0f, 300.0f);

                    cells[i].fx -= dx * (f_mag / dist);
                    cells[i].fy -= dy * (f_mag / dist);
                    cells[i].fz -= dz * (f_mag / dist);

                    cells[j].fx += dx * (f_mag / dist);
                    cells[j].fy += dy * (f_mag / dist);
                    cells[j].fz += dz * (f_mag / dist);
                }
            }
        }

        // 3. 突触弹簧引力场计算 (有向结构张力)
        std::unordered_map<uint16_t, size_t> id_map;
        for (size_t i = 0; i < cells.size(); ++i) id_map[cells[i].id] = i;

        for (auto& syn : synapses) {
            if (!syn.is_active) continue;
            auto it1 = id_map.find(syn.from_cell_id);
            auto it2 = id_map.find(syn.to_cell_id);
            if (it1 != id_map.end() && it2 != id_map.end()) {
                auto& c1 = cells[it1->second];
                auto& c2 = cells[it2->second];

                float dx = c2.x - c1.x;
                float dy = c2.y - c1.y;
                float dz = c2.z - c1.z;
                float dist = std::sqrt(dx * dx + dy * dy + dz * dz + 1e-4f);

                float delta = dist - syn.rest_length;
                float spring_f = k_spring * delta;

                float nx = dx / dist;
                float ny = dy / dist;
                float nz = dz / dist;

                c1.fx += nx * spring_f;
                c1.fy += ny * spring_f;
                c1.fz += nz * spring_f;

                c2.fx -= nx * spring_f;
                c2.fy -= ny * spring_f;
                c2.fz -= nz * spring_f;

                // 推进神经放电光子动画
                if (syn.photon_pos >= 0.0f) {
                    syn.photon_pos += dt * 3.0f;
                    if (syn.photon_pos > 1.0f) syn.photon_pos = -1.0f;
                }
            }
        }

        // 4. 牛顿第二定律积分推进 (Verlet Velocity Step)
        for (auto& c : cells) {
            c.vx = (c.vx + c.fx * dt) * damping;
            c.vy = (c.vy + c.fy * dt) * damping;
            c.vz = (c.vz + c.fz * dt) * damping;

            c.x += c.vx * dt;
            c.y += c.vy * dt;
            c.z += c.vz * dt;
        }
    }

    // 运行时代谢传导前向计算 (Forward Pass: 真·零 GC，纯连续数组计算)
    struct ActionOutputs {
        double positive_action{0.0}; // 买入/加速
        double negative_action{0.0}; // 卖出/制动
        double defensive_reset{0.0}; // 清仓/居中
        bool   immune_lock{false};   // 免疫熔断
    };

    ActionOutputs forward(const double inputs[4]) {
        if (!is_compiled_) compile();

        // 1. 清空扁平输入端口缓冲
        std::fill(flat_port_inputs_.begin(), flat_port_inputs_.end(), 0.0);

        // 2. 突触快速汇聚 (连续内存线性遍历)
        for (const auto& syn : compiled_synapses_) {
            double val = cells[syn.from_idx].output_val * syn.weight;
            flat_port_inputs_[syn.to_idx * 2 + syn.to_port] += val;
        }

        // 3. 按预编译拓扑顺序逐一激发细胞
        for (size_t idx : execution_order_) {
            auto& c = cells[idx];
            double in0 = flat_port_inputs_[idx * 2 + 0];
            double in1 = flat_port_inputs_[idx * 2 + 1];

            switch (c.type) {
                case CellType::SENSE_RAW_INPUT_0: c.output_val = inputs[0] * c.param1; break;
                case CellType::SENSE_RAW_INPUT_1: c.output_val = inputs[1] * c.param1; break;
                case CellType::SENSE_RAW_INPUT_2: c.output_val = inputs[2] * c.param1; break;
                case CellType::SENSE_RAW_INPUT_3: c.output_val = inputs[3] * c.param1; break;

                case CellType::OP_EMA: {
                    double alpha = std::clamp(c.param1, 0.001, 1.0);
                    if (c.activation_count == 0) c.state_val = in0;
                    else c.state_val = alpha * in0 + (1.0 - alpha) * c.state_val;
                    c.output_val = c.state_val;
                    break;
                }
                case CellType::OP_DIFF:
                    c.output_val = in0 - c.prev_input;
                    c.prev_input = in0;
                    break;
                case CellType::OP_INTEGRAL:
                    c.state_val += in0 * c.param1;
                    c.output_val = c.state_val;
                    break;
                case CellType::OP_SUM:
                    c.output_val = in0 + in1;
                    break;
                case CellType::OP_SUB:
                    c.output_val = in0 - in1;
                    break;
                case CellType::OP_MULTIPLY:
                    c.output_val = in0 * in1;
                    break;
                case CellType::OP_RATIO:
                    c.output_val = in0 / (std::abs(in1) > 1e-6 ? in1 : 1e-6);
                    break;
                case CellType::OP_ABS:
                    c.output_val = std::abs(in0);
                    break;

                case CellType::GATE_THRESHOLD:
                    c.output_val = (in0 > c.param1) ? 1.0 : 0.0;
                    break;
                case CellType::GATE_HYSTERESIS:
                    if (in0 > c.param1) c.latch_state = true;
                    else if (in0 < c.param2) c.latch_state = false;
                    c.output_val = c.latch_state ? 1.0 : -1.0;
                    break;
                case CellType::GATE_AND:
                    c.output_val = (in0 > 0.0 && in1 > 0.0) ? 1.0 : 0.0;
                    break;
                case CellType::GATE_INHIBIT:
                    c.output_val = in0 * std::max(0.0, 1.0 - in1);
                    break;

                case CellType::ACT_PRIMARY_POSITIVE:
                case CellType::ACT_PRIMARY_NEGATIVE:
                case CellType::ACT_DEFENSIVE_RESET:
                case CellType::ACT_IMMUNE_BLOCK:
                    c.output_val = in0;
                    break;
            }

            if (std::abs(c.output_val) > 1e-6) {
                c.activation_count++;
                c.glow_charge = std::min(1.0f, c.glow_charge + 0.3f);
            }
        }

        // 收集最终动作信号
        ActionOutputs actions{};
        for (const auto& c : cells) {
            if (c.type == CellType::ACT_PRIMARY_POSITIVE) actions.positive_action = c.output_val;
            else if (c.type == CellType::ACT_PRIMARY_NEGATIVE) actions.negative_action = c.output_val;
            else if (c.type == CellType::ACT_DEFENSIVE_RESET) actions.defensive_reset = c.output_val;
            else if (c.type == CellType::ACT_IMMUNE_BLOCK) {
                if (c.output_val > 0.5) actions.immune_lock = true;
            }
        }
        return actions;
    }

    // 导出全息 3D 可视化 JSON 数据帧 (包含物理坐标与发光电位)
    std::string to_json() const {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"organism_id\": " << organism_id << ",\n";
        ss << "  \"generation\": " << generation << ",\n";
        ss << "  \"lineage\": \"" << lineage_name << "\",\n";
        ss << "  \"fitness\": " << fitness_score << ",\n";
        ss << "  \"cells\": [\n";
        for (size_t i = 0; i < cells.size(); ++i) {
            const auto& c = cells[i];
            ss << "    {\"id\": " << c.id << ", \"type\": \"" << to_string(c.type) << "\", "
               << "\"param1\": " << c.param1 << ", \"param2\": " << c.param2 << ", "
               << "\"output\": " << c.output_val << ", \"activations\": " << c.activation_count << ", "
               << "\"x\": " << c.x << ", \"y\": " << c.y << ", \"z\": " << c.z << ", "
               << "\"glow\": " << c.glow_charge << "}"
               << (i + 1 < cells.size() ? "," : "") << "\n";
        }
        ss << "  ],\n";
        ss << "  \"synapses\": [\n";
        for (size_t i = 0; i < synapses.size(); ++i) {
            const auto& s = synapses[i];
            ss << "    {\"from\": " << s.from_cell_id << ", \"to\": " << s.to_cell_id << ", "
               << "\"port\": " << (int)s.to_port << ", \"weight\": " << s.weight << ", "
               << "\"active\": " << (s.is_active ? "true" : "false") << ", "
               << "\"photon_pos\": " << s.photon_pos << "}"
               << (i + 1 < synapses.size() ? "," : "") << "\n";
        }
        ss << "  ]\n";
        ss << "}";
        return ss.str();
    }
};

// ============================================================================
// 6. 形态发生进化引擎 (MorphogeneticEvolutionEngine)
// ============================================================================
class MorphogeneticEvolutionEngine {
public:
    explicit MorphogeneticEvolutionEngine(size_t population_size = 20, uint32_t seed = 42)
        : rng_(seed), population_size_(population_size) {
        init_population();
    }

    void init_population() {
        population_.clear();
        for (size_t i = 0; i < population_size_; ++i) {
            auto org = CellularOrganism::create_seed_organism(i + 1);
            if (i > 0) {
                for (int m = 0; m < 3; ++m) mutate(org);
            }
            population_.push_back(std::move(org));
        }
    }

    // ── 生物变异操作 1: 细胞分裂增殖 (Mitosis / Add Cell) ──
    bool mutate_add_cell(CellularOrganism& org) {
        if (org.synapses.empty()) return false;
        std::uniform_int_distribution<size_t> dist_syn(0, org.synapses.size() - 1);
        size_t s_idx = dist_syn(rng_);
        auto& old_syn = org.synapses[s_idx];
        if (!old_syn.is_active) return false;

        static const CellType candidates[] = {
            CellType::OP_EMA, CellType::OP_DIFF, CellType::OP_SUM, CellType::OP_SUB,
            CellType::OP_RATIO, CellType::GATE_THRESHOLD, CellType::GATE_HYSTERESIS,
            CellType::GATE_AND, CellType::GATE_INHIBIT
        };
        std::uniform_int_distribution<size_t> dist_type(0, sizeof(candidates) / sizeof(candidates[0]) - 1);
        CellType new_type = candidates[dist_type(rng_)];

        uint16_t new_id = 0;
        for (const auto& c : org.cells) new_id = std::max(new_id, c.id);
        new_id += 1;

        std::uniform_real_distribution<double> dist_param(0.01, 1.0);
        std::uniform_real_distribution<float> dist_pos(-30.0f, 30.0f);

        Cell new_cell{new_id, new_type, dist_param(rng_), -dist_param(rng_), 0.0, 0.0, false, 0.0, 0, 0,
                      dist_pos(rng_), dist_pos(rng_), 0.0f};
        org.cells.push_back(new_cell);

        uint16_t from_id = old_syn.from_cell_id;
        uint16_t to_id = old_syn.to_cell_id;
        uint8_t to_port = old_syn.to_port;
        double orig_weight = old_syn.weight;
        old_syn.is_active = false;

        org.synapses.push_back({from_id, new_id, 0, 1.0, true, 60.0f, -1.0f});
        org.synapses.push_back({new_id, to_id, to_port, orig_weight, true, 60.0f, -1.0f});

        org.compile();
        return true;
    }

    // ── 生物变异操作 2: 突触跨界重连 (Synaptic Rewiring) ──
    bool mutate_add_synapse(CellularOrganism& org) {
        if (org.cells.size() < 2) return false;
        std::uniform_int_distribution<size_t> dist_cell(0, org.cells.size() - 1);
        size_t idx_a = dist_cell(rng_);
        size_t idx_b = dist_cell(rng_);
        if (idx_a == idx_b) return false;

        uint16_t from_id = org.cells[idx_a].id;
        uint16_t to_id = org.cells[idx_b].id;

        if (org.cells[idx_b].type == CellType::SENSE_RAW_INPUT_0 ||
            org.cells[idx_b].type == CellType::SENSE_RAW_INPUT_1 ||
            org.cells[idx_b].type == CellType::SENSE_RAW_INPUT_2 ||
            org.cells[idx_b].type == CellType::SENSE_RAW_INPUT_3) {
            return false;
        }

        std::uniform_real_distribution<double> dist_weight(-2.0, 2.0);
        std::uniform_int_distribution<int> dist_port(0, 1);

        org.synapses.push_back({from_id, to_id, static_cast<uint8_t>(dist_port(rng_)), dist_weight(rng_), true, 60.0f, -1.0f});
        org.compile();
        return true;
    }

    // ── 生物变异操作 3: 细胞参数微调 (Metabolic Drift) ──
    bool mutate_parameters(CellularOrganism& org) {
        if (org.cells.empty()) return false;
        std::uniform_int_distribution<size_t> dist_cell(0, org.cells.size() - 1);
        auto& c = org.cells[dist_cell(rng_)];

        std::normal_distribution<double> dist_noise(0.0, 0.05);
        c.param1 = std::clamp(c.param1 + dist_noise(rng_), -5.0, 5.0);
        c.param2 = std::clamp(c.param2 + dist_noise(rng_), -5.0, 5.0);
        return true;
    }

    // ── 生物自我净化: 细胞凋亡与剪枝 (Apoptosis / Pruning) ──
    void prune_apoptosis(CellularOrganism& org) {
        std::unordered_set<uint16_t> useful_cells;
        for (const auto& c : org.cells) {
            if (c.type == CellType::ACT_PRIMARY_POSITIVE ||
                c.type == CellType::ACT_PRIMARY_NEGATIVE ||
                c.type == CellType::ACT_DEFENSIVE_RESET ||
                c.type == CellType::ACT_IMMUNE_BLOCK) {
                useful_cells.insert(c.id);
            }
        }

        bool expanded = true;
        while (expanded) {
            expanded = false;
            for (const auto& syn : org.synapses) {
                if (syn.is_active && useful_cells.count(syn.to_cell_id) && !useful_cells.count(syn.from_cell_id)) {
                    useful_cells.insert(syn.from_cell_id);
                    expanded = true;
                }
            }
        }

        org.cells.erase(
            std::remove_if(org.cells.begin(), org.cells.end(), [&](const Cell& c) {
                if (c.type == CellType::SENSE_RAW_INPUT_0 ||
                    c.type == CellType::SENSE_RAW_INPUT_1 ||
                    c.type == CellType::SENSE_RAW_INPUT_2 ||
                    c.type == CellType::SENSE_RAW_INPUT_3) return false;
                return !useful_cells.count(c.id);
            }),
            org.cells.end()
        );

        std::unordered_set<uint16_t> live_ids;
        for (const auto& c : org.cells) live_ids.insert(c.id);

        org.synapses.erase(
            std::remove_if(org.synapses.begin(), org.synapses.end(), [&](const Synapse& s) {
                return !s.is_active || !live_ids.count(s.from_cell_id) || !live_ids.count(s.to_cell_id);
            }),
            org.synapses.end()
        );

        org.compile();
    }

    // 综合变异入口
    void mutate(CellularOrganism& org) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        double r = dist(rng_);
        if (r < 0.35) mutate_parameters(org);
        else if (r < 0.70) mutate_add_synapse(org);
        else mutate_add_cell(org);

        if (dist(rng_) < 0.05) prune_apoptosis(org);
    }

    // ── 种群世代演化 (Evolve Next Generation) ──
    void evolve_generation() {
        std::sort(population_.begin(), population_.end(), [](const CellularOrganism& a, const CellularOrganism& b) {
            return a.fitness_score > b.fitness_score;
        });

        size_t elite_count = std::max<size_t>(1, population_size_ / 4);
        std::vector<CellularOrganism> next_gen;

        for (size_t i = 0; i < elite_count; ++i) {
            next_gen.push_back(population_[i]);
        }

        std::uniform_int_distribution<size_t> dist_parent(0, elite_count - 1);
        while (next_gen.size() < population_size_) {
            size_t p_idx = dist_parent(rng_);
            CellularOrganism child = population_[p_idx];
            child.organism_id = next_gen.size() + 1;
            child.generation = population_[p_idx].generation + 1;
            child.lineage_name = "Apex-Gen" + std::to_string(child.generation);
            mutate(child);
            next_gen.push_back(std::move(child));
        }

        population_ = std::move(next_gen);
    }

    CellularOrganism& get_champion() { return population_[0]; }
    const std::vector<CellularOrganism>& get_population() const { return population_; }
    std::vector<CellularOrganism>& population() { return population_; }  // 任务训练器写入适应度用

private:
    std::mt19937 rng_;
    size_t population_size_{20};
    std::vector<CellularOrganism> population_;
};

} // namespace kun
