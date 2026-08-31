#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <random>
#include <memory>
#include <sstream>
#include <fstream>
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
    OP_DELAY_N = 18,         // 延迟管道 (Sliding FIFO delay u(t) = x(t-k))
    OP_OSCILLATOR = 19,      // 范德波尔相弛豫振荡器 (Van der Pol Oscillator)
    OP_QUADRATIC = 20,       // 二次能量型 (Quadratic Lyapunov energy form)
    
    // 【门控逻辑神经元 (Gating / Threshold Neurons)】
    GATE_THRESHOLD = 24,     // 阶跃阈值激活 (Input > param ? 1 : 0)
    GATE_HYSTERESIS = 25,    // 迟滞比较器 (Schmitt Trigger，防震荡高频抖动)
    GATE_AND = 26,           // 协同兴奋门 (Input A > 0 && Input B > 0)
    GATE_INHIBIT = 27,       // 抑制性突触 (Input A * (1.0 - Input B))
    GATE_DEADZONE = 28,      // 中心死区噪声门 (Central deadband noise gate)
    GATE_MIN_MAX = 29,       // 极值包络门 (Extremum envelope gate)
    
    // 【效应/动作细胞 (Effector / Action)】
    ACT_PRIMARY_POSITIVE = 30, // 正向激发动作 (量化: 买开仓 / 智驾: 变道加速)
    ACT_PRIMARY_NEGATIVE = 31, // 反向激发动作 (量化: 卖开仓 / 智驾: 减速避让)
    ACT_DEFENSIVE_RESET  = 32, // 防御性归零 (量化: 平仓清空 / 智驾: 保持车道居中)
    ACT_IMMUNE_BLOCK     = 33, // 免疫阻断刹车 (量化: 熔断锁定 / 智驾: AEB紧急制动)

    // 【认知联络与预测受体 (Cognitive & Predictive World Model)】
    PREDICT_SENSE_0      = 40, // 内部前瞻预测受体 0 (预测下一时刻输入0, 内部世界模型输出)
    PREDICT_SENSE_1      = 41, // 内部前瞻预测受体 1 (预测下一时刻输入1)
    ASSOCIATION_HUB      = 42  // 联络皮层中枢细胞 (皮层柱联想聚类, 概念吸引子表征)
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
        case CellType::OP_DELAY_N: return "Op_DelayN";
        case CellType::OP_OSCILLATOR: return "Op_Oscillator";
        case CellType::OP_QUADRATIC: return "Op_Quadratic";
        case CellType::GATE_THRESHOLD: return "Gate_Threshold";
        case CellType::GATE_HYSTERESIS: return "Gate_Hysteresis";
        case CellType::GATE_AND: return "Gate_And";
        case CellType::GATE_INHIBIT: return "Gate_Inhibit";
        case CellType::GATE_DEADZONE: return "Gate_Deadzone";
        case CellType::GATE_MIN_MAX: return "Gate_MinMax";
        case CellType::ACT_PRIMARY_POSITIVE: return "Act_PosAction";
        case CellType::ACT_PRIMARY_NEGATIVE: return "Act_NegAction";
        case CellType::ACT_DEFENSIVE_RESET: return "Act_DefReset";
        case CellType::ACT_IMMUNE_BLOCK: return "Act_ImmuneLock";
        case CellType::PREDICT_SENSE_0: return "Pred_Sense0";
        case CellType::PREDICT_SENSE_1: return "Pred_Sense1";
        case CellType::ASSOCIATION_HUB: return "Assoc_Hub";
        default: return "Cell_Unknown";
    }
}

inline CellType cell_type_from_string(const std::string& name) {
    if (name == "Sense_Input0") return CellType::SENSE_RAW_INPUT_0;
    if (name == "Sense_Input1") return CellType::SENSE_RAW_INPUT_1;
    if (name == "Sense_Input2") return CellType::SENSE_RAW_INPUT_2;
    if (name == "Sense_Input3") return CellType::SENSE_RAW_INPUT_3;
    if (name == "Op_EMA") return CellType::OP_EMA;
    if (name == "Op_Diff") return CellType::OP_DIFF;
    if (name == "Op_Integral") return CellType::OP_INTEGRAL;
    if (name == "Op_Sum") return CellType::OP_SUM;
    if (name == "Op_Sub") return CellType::OP_SUB;
    if (name == "Op_Multiply") return CellType::OP_MULTIPLY;
    if (name == "Op_Ratio") return CellType::OP_RATIO;
    if (name == "Op_Abs") return CellType::OP_ABS;
    if (name == "Op_DelayN") return CellType::OP_DELAY_N;
    if (name == "Op_Oscillator") return CellType::OP_OSCILLATOR;
    if (name == "Op_Quadratic") return CellType::OP_QUADRATIC;
    if (name == "Gate_Threshold") return CellType::GATE_THRESHOLD;
    if (name == "Gate_Hysteresis") return CellType::GATE_HYSTERESIS;
    if (name == "Gate_And") return CellType::GATE_AND;
    if (name == "Gate_Inhibit") return CellType::GATE_INHIBIT;
    if (name == "Gate_Deadzone") return CellType::GATE_DEADZONE;
    if (name == "Gate_MinMax") return CellType::GATE_MIN_MAX;
    if (name == "Act_PosAction") return CellType::ACT_PRIMARY_POSITIVE;
    if (name == "Act_NegAction") return CellType::ACT_PRIMARY_NEGATIVE;
    if (name == "Act_DefReset") return CellType::ACT_DEFENSIVE_RESET;
    if (name == "Act_ImmuneLock") return CellType::ACT_IMMUNE_BLOCK;
    if (name == "Pred_Sense0") return CellType::PREDICT_SENSE_0;
    if (name == "Pred_Sense1") return CellType::PREDICT_SENSE_1;
    if (name == "Assoc_Hub") return CellType::ASSOCIATION_HUB;
    return CellType::OP_EMA;
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

    // === 终身可塑性与递归循环动力学参数 (Lifelong Plasticity & Recurrent Dynamics) ===
    double initial_weight{1.0}; // 基因始祖基础权重
    double hebbian_rate{0.005}; // 赫布/Oja 在线学习率 eta
    double hebbian_decay{0.02}; // Oja 自归一化衰减系数 alpha
    bool   is_recurrent{false}; // 是否为时序反馈循环突触
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

    // === 高阶动态与零 GC 缓冲状态 (编译期内嵌连续内存，绝无堆分配) ===
    double aux_state{0.0};                 // 辅助内部动力学状态 (如 OP_OSCILLATOR 的速度变量 s2)
    double delay_buffer[16]{0.0};          // 静态滑动 FIFO 管道缓冲 (用于 OP_DELAY_N)
    uint8_t delay_idx{0};                  // FIFO 环形缓冲区游标 (用于 OP_DELAY_N)
    double prev_output_val{0.0};           // 上一时刻时序输出 (用于递归循环 1-step delay)

    const char* type_name() const {
        return to_string(type);
    }
};

// ============================================================================
// 3.5 初始胚胎发生模式 (Seed Initialization Mode)
// ============================================================================
enum class SeedInitMode : uint8_t {
    HANDCRAFTED_PROGENITOR = 0, // Mode A: 9-cell Genesis seed (2 receptors, 3 metabolic, 1 gating, 3 effectors, 7 synapses)
    MINIMAL_RANDOM_GRAPH = 1,   // Mode B: 3-4 cells (2 receptors + 1 random metabolic/gating + 1-2 effectors with random weights U(-2, 2))
    DISCONNECTED_EMBRYO = 2     // Mode C: Pure receptors and effectors with no intermediate connections at gen-0
};

inline const char* to_string(SeedInitMode mode) {
    switch (mode) {
        case SeedInitMode::HANDCRAFTED_PROGENITOR: return "Handcrafted_Progenitor";
        case SeedInitMode::MINIMAL_RANDOM_GRAPH: return "Minimal_Random_Graph";
        case SeedInitMode::DISCONNECTED_EMBRYO: return "Disconnected_Embryo";
        default: return "Unknown_Mode";
    }
}

// ── 演化约束消融实验配置 (Evolution Constraint Ablation Strategy) ──
enum class SkeletonLockMode : uint8_t {
    LOCKED = 0,    // 感受器与效应器严格不可修改/增删 (结构受限骨架)
    UNLOCKED = 1   // 感受器与效应器自由变异、增殖与凋亡 (自由开放形态发生)
};

enum class TypeWhitelistMode : uint8_t {
    CURATED_9 = 0, // 9 种基础代谢与门控算子白名单
    FULL_24 = 1    // 24 种全原语分类学 (完整算子空间)
};

enum class FitnessDriverMode : uint8_t {
    TASK_FITNESS_ONLY = 0, // 纯外部任务目标打分
    NOVELTY_SEARCH = 1,    // 纯内在动机：行为空间 KNN 稀缺度搜索
    HYBRID_CURIOSITY = 2   // 混合驱动：任务适应度 + 好奇心新颖性奖励
};

struct EvolutionConstraintConfig {
    SkeletonLockMode skeleton_lock{SkeletonLockMode::LOCKED};
    TypeWhitelistMode type_whitelist{TypeWhitelistMode::FULL_24};
    SeedInitMode seed_mode{SeedInitMode::HANDCRAFTED_PROGENITOR};
    FitnessDriverMode fitness_driver{FitnessDriverMode::TASK_FITNESS_ONLY};
    double novelty_weight{0.3}; // 好奇心奖励权重 alpha
};

inline const char* to_string(SkeletonLockMode m) {
    return (m == SkeletonLockMode::LOCKED) ? "Skeleton_Locked" : "Skeleton_Unlocked";
}

inline const char* to_string(TypeWhitelistMode m) {
    return (m == TypeWhitelistMode::CURATED_9) ? "Curated_9_Primitives" : "Full_24_Primitives";
}

inline const char* to_string(FitnessDriverMode m) {
    switch (m) {
        case FitnessDriverMode::TASK_FITNESS_ONLY: return "Task_Fitness_Only";
        case FitnessDriverMode::NOVELTY_SEARCH: return "Novelty_Search";
        case FitnessDriverMode::HYBRID_CURIOSITY: return "Hybrid_Curiosity";
        default: return "Unknown_Fitness_Driver";
    }
}

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
        double initial_weight;
        double hebbian_rate;
        double hebbian_decay;
        bool   is_recurrent;
    };
    struct CompiledActionCell {
        size_t cell_idx;
        CellType type;
    };
    std::vector<CompiledSynapse> compiled_synapses_;
    std::vector<CompiledActionCell> compiled_actions_;
    std::vector<size_t> execution_order_;
    mutable std::vector<double> flat_port_inputs_; // [cell_idx * 2 + port]
    bool is_compiled_{false};
    bool is_compiled() const { return is_compiled_; }

    CellularOrganism() = default;

    // 回合间状态重置: 清零动态膜电位与记忆, 保留基因组 (参数/拓扑/坐标) 不变。
    // reset_plasticity: 是否将在线学习调整的突触权重重置回初始基因组权重
    void reset_state(bool reset_plasticity = false) {
        for (auto& c : cells) {
            c.state_val = 0.0;
            c.prev_input = 0.0;
            c.latch_state = false;
            c.output_val = 0.0;
            c.prev_output_val = 0.0;
            c.aux_state = 0.0;
            std::fill(std::begin(c.delay_buffer), std::end(c.delay_buffer), 0.0);
            c.delay_idx = 0;
        }
        std::fill(flat_port_inputs_.begin(), flat_port_inputs_.end(), 0.0);

        if (reset_plasticity) {
            for (auto& s : compiled_synapses_) {
                s.weight = s.initial_weight;
            }
            for (auto& s : synapses) {
                s.weight = s.initial_weight;
            }
        }
    }

    // 创建最简单细胞原生生物 (Archean Progenitor / Mode A: Handcrafted Progenitor)
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

    static CellularOrganism create_handcrafted_progenitor(uint64_t id = 1) {
        return create_seed_organism(id);
    }

    // Mode B: 最小随机图结构 (Minimal Random Graph: 3-4 cells: 2 receptors + 1 random metabolic/gating cell + 1 effector with random weights U(-2, 2))
    static CellularOrganism create_minimal_random_graph(uint64_t id = 1, uint32_t seed = 42) {
        CellularOrganism org;
        org.organism_id = id;
        org.generation = 0;
        org.lineage_name = "MinimalRandom-" + std::to_string(id);

        std::mt19937 prng(seed + static_cast<uint32_t>(id * 1013));
        std::uniform_real_distribution<double> dist_w(-2.0, 2.0);
        std::uniform_real_distribution<double> dist_param(-1.0, 1.0);

        static const CellType candidates[] = {
            CellType::OP_EMA, CellType::OP_DIFF, CellType::OP_INTEGRAL,
            CellType::OP_SUM, CellType::OP_SUB, CellType::OP_MULTIPLY,
            CellType::OP_RATIO, CellType::OP_ABS,
            CellType::OP_DELAY_N, CellType::OP_OSCILLATOR, CellType::OP_QUADRATIC,
            CellType::GATE_THRESHOLD, CellType::GATE_HYSTERESIS,
            CellType::GATE_AND, CellType::GATE_INHIBIT,
            CellType::GATE_DEADZONE, CellType::GATE_MIN_MAX
        };
        std::uniform_int_distribution<size_t> dist_type(0, sizeof(candidates) / sizeof(candidates[0]) - 1);
        CellType mid_type = candidates[dist_type(prng)];

        // 2 感知受体细胞
        org.cells.push_back({0, CellType::SENSE_RAW_INPUT_0, 1.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, -100.0f, -30.0f, 0.0f});
        org.cells.push_back({1, CellType::SENSE_RAW_INPUT_1, 1.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, -100.0f,  30.0f, 0.0f});

        // 1 随机代谢/门控细胞
        org.cells.push_back({2, mid_type, dist_param(prng), dist_param(prng), 0.0, 0.0, false, 0.0, 0, 0, 0.0f, 0.0f, 0.0f});

        // 效应动作细胞 (3-4 cells: 1 积极动作 + 1 消极动作)
        org.cells.push_back({3, CellType::ACT_PRIMARY_POSITIVE, 0.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, 100.0f, -30.0f, 0.0f});
        org.cells.push_back({4, CellType::ACT_PRIMARY_NEGATIVE, 0.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, 100.0f,  30.0f, 0.0f});

        // 随机突触连接 U(-2, 2)
        std::uniform_int_distribution<int> dist_sense(0, 1);
        uint16_t s_id = static_cast<uint16_t>(dist_sense(prng));
        org.synapses.push_back({s_id, 2, 0, dist_w(prng), true, 60.0f, -1.0f});
        org.synapses.push_back({static_cast<uint16_t>(1 - s_id), 2, 1, dist_w(prng), true, 60.0f, -1.0f});
        org.synapses.push_back({2, 3, 0, dist_w(prng), true, 60.0f, -1.0f});
        org.synapses.push_back({2, 4, 0, dist_w(prng), true, 60.0f, -1.0f});

        org.compile();
        return org;
    }

    // Mode C: 无连接原始胚胎 (Disconnected Embryo: pure receptors and effectors with no intermediate connections at gen-0)
    static CellularOrganism create_disconnected_embryo(uint64_t id = 1) {
        CellularOrganism org;
        org.organism_id = id;
        org.generation = 0;
        org.lineage_name = "Embryo-" + std::to_string(id);

        // 纯感知受体细胞
        org.cells.push_back({0, CellType::SENSE_RAW_INPUT_0, 1.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, -120.0f, -60.0f, 0.0f});
        org.cells.push_back({1, CellType::SENSE_RAW_INPUT_1, 1.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, -120.0f, -20.0f, 0.0f});
        org.cells.push_back({2, CellType::SENSE_RAW_INPUT_2, 1.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, -120.0f,  20.0f, 0.0f});
        org.cells.push_back({3, CellType::SENSE_RAW_INPUT_3, 1.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, -120.0f,  60.0f, 0.0f});

        // 纯效应动作细胞
        org.cells.push_back({4, CellType::ACT_PRIMARY_POSITIVE, 0.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, 120.0f, -60.0f, 0.0f});
        org.cells.push_back({5, CellType::ACT_PRIMARY_NEGATIVE, 0.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, 120.0f, -20.0f, 0.0f});
        org.cells.push_back({6, CellType::ACT_DEFENSIVE_RESET,  0.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, 120.0f,  20.0f, 0.0f});
        org.cells.push_back({7, CellType::ACT_IMMUNE_BLOCK,     0.0, 0.0, 0.0, 0.0, false, 0.0, 0, 0, 120.0f,  60.0f, 0.0f});

        // 突触完全为空 (0 connections at gen-0)
        org.synapses.clear();

        org.compile();
        return org;
    }

    // 通用模式工厂分发器
    static CellularOrganism create_by_mode(SeedInitMode mode, uint64_t id = 1, uint32_t seed = 42) {
        switch (mode) {
            case SeedInitMode::HANDCRAFTED_PROGENITOR:
                return create_handcrafted_progenitor(id);
            case SeedInitMode::MINIMAL_RANDOM_GRAPH:
                return create_minimal_random_graph(id, seed);
            case SeedInitMode::DISCONNECTED_EMBRYO:
                return create_disconnected_embryo(id);
        }
        return create_seed_organism(id);
    }

    // 拓扑排序与扁平执行编译 (Flat Array Compilation: 消除所有运行期堆分配)
    bool compile() {
        std::unordered_map<uint16_t, size_t> id_to_idx;
        for (size_t i = 0; i < cells.size(); ++i) {
            id_to_idx[cells[i].id] = i;
        }

        std::unordered_map<uint16_t, int> in_degrees;
        std::unordered_map<uint16_t, std::vector<uint16_t>> adj;
        std::unordered_map<uint16_t, std::vector<uint16_t>> rev_adj;
        for (const auto& c : cells) in_degrees[c.id] = 0;

        for (const auto& syn : synapses) {
            if (!syn.is_active) continue;
            auto it_from = id_to_idx.find(syn.from_cell_id);
            auto it_to = id_to_idx.find(syn.to_cell_id);
            if (it_from != id_to_idx.end() && it_to != id_to_idx.end()) {
                adj[syn.from_cell_id].push_back(syn.to_cell_id);
                rev_adj[syn.to_cell_id].push_back(syn.from_cell_id);
                in_degrees[syn.to_cell_id]++;
            }
        }

        // 识别动作细胞与预测受体细胞
        compiled_actions_.clear();
        std::unordered_set<uint16_t> active_cell_ids;
        std::vector<uint16_t> rev_queue;

        for (size_t i = 0; i < cells.size(); ++i) {
            if (cells[i].type == CellType::ACT_PRIMARY_POSITIVE ||
                cells[i].type == CellType::ACT_PRIMARY_NEGATIVE ||
                cells[i].type == CellType::ACT_DEFENSIVE_RESET ||
                cells[i].type == CellType::ACT_IMMUNE_BLOCK ||
                cells[i].type == CellType::PREDICT_SENSE_0 ||
                cells[i].type == CellType::PREDICT_SENSE_1) {
                compiled_actions_.push_back({i, cells[i].type});
                active_cell_ids.insert(cells[i].id);
                rev_queue.push_back(cells[i].id);
            }
        }

        // 若暂无动作细胞（如刚初始化的胚胎），则默认全细胞保留
        if (compiled_actions_.empty()) {
            for (const auto& c : cells) active_cell_ids.insert(c.id);
        } else {
            // 反向活性分析 (从动作细胞反向追溯所有有贡献的细胞)
            size_t rev_head = 0;
            while (rev_head < rev_queue.size()) {
                uint16_t curr = rev_queue[rev_head++];
                auto it = rev_adj.find(curr);
                if (it != rev_adj.end()) {
                    for (uint16_t parent : it->second) {
                        if (active_cell_ids.insert(parent).second) {
                            rev_queue.push_back(parent);
                        }
                    }
                }
            }
        }

        // 拓扑排序 (Kahn's Algorithm)
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
            if (active_cell_ids.find(u) != active_cell_ids.end()) {
                execution_order_.push_back(id_to_idx[u]);
            }

            for (uint16_t v : adj[u]) {
                in_degrees[v]--;
                if (in_degrees[v] == 0) {
                    queue.push_back(v);
                }
            }
        }

        // 环保护补齐
        if (execution_order_.size() < active_cell_ids.size()) {
            std::unordered_set<size_t> visited(execution_order_.begin(), execution_order_.end());
            for (uint16_t cid : active_cell_ids) {
                size_t idx = id_to_idx[cid];
                if (visited.find(idx) == visited.end()) {
                    execution_order_.push_back(idx);
                }
            }
        }

        // 建立执行顺序位阶索引 (用于一等公民循环连接与前向/反馈突触判定)
        std::vector<size_t> cell_rank(cells.size(), 999999);
        for (size_t r = 0; r < execution_order_.size(); ++r) {
            cell_rank[execution_order_[r]] = r;
        }

        // 编译有效突触 (自动区分前向突触与递归反馈突触)
        compiled_synapses_.clear();
        for (const auto& syn : synapses) {
            if (!syn.is_active) continue;
            if (active_cell_ids.find(syn.from_cell_id) == active_cell_ids.end() ||
                active_cell_ids.find(syn.to_cell_id) == active_cell_ids.end()) {
                continue;
            }
            auto it_from = id_to_idx.find(syn.from_cell_id);
            auto it_to = id_to_idx.find(syn.to_cell_id);
            if (it_from != id_to_idx.end() && it_to != id_to_idx.end()) {
                size_t f_idx = it_from->second;
                size_t t_idx = it_to->second;
                bool is_loop = (cell_rank[f_idx] >= cell_rank[t_idx]); // 发送端位阶>=接收端位阶即为递归时序环

                compiled_synapses_.push_back({
                    f_idx, t_idx, syn.to_port,
                    syn.weight, syn.weight,
                    syn.hebbian_rate, syn.hebbian_decay,
                    is_loop
                });
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
        const float r_cut_sq = r_cut * r_cut;
        const float k_spring = 0.05f;      // 突触结构弹簧系数
        const float damping = 0.85f;       // 黏性阻尼

        // 1. 重置合力并衰减发光电位
        for (auto& c : cells) {
            c.fx = 0.0f; c.fy = 0.0f; c.fz = 0.0f;
            c.glow_charge *= 0.92f;
        }

        // 2. 严格 12-6 兰纳-琼斯多体非键结势能力场 (3D 近斥中吸远无 + 空间截断优化)
        const size_t num_cells = cells.size();
        for (size_t i = 0; i < num_cells; ++i) {
            auto& ci = cells[i];
            for (size_t j = i + 1; j < num_cells; ++j) {
                auto& cj = cells[j];
                float dx = cj.x - ci.x;
                float dy = cj.y - ci.y;
                float dz = cj.z - ci.z;
                float dist_sq = dx * dx + dy * dy + dz * dz + 1e-4f;

                if (dist_sq < r_cut_sq && dist_sq > 1.0f) {
                    float dist = std::sqrt(dist_sq);
                    float r2 = dist_sq;
                    float r6 = r2 * r2 * r2;
                    float r12 = r6 * r6;

                    // 12-6 势解析导数力标量: f_mag > 0 为斥力, f_mag < 0 为吸引力
                    float sr6 = sigma6 / r6;
                    float sr12 = sigma12 / r12;
                    float f_mag = (24.0f * epsilon / r2) * (2.0f * sr12 - sr6);

                    // 避免极端近距离数值发散溢出 (Force Cap)
                    f_mag = std::clamp(f_mag, -50.0f, 300.0f);
                    float inv_dist = f_mag / dist;

                    ci.fx -= dx * inv_dist;
                    ci.fy -= dy * inv_dist;
                    ci.fz -= dz * inv_dist;

                    cj.fx += dx * inv_dist;
                    cj.fy += dy * inv_dist;
                    cj.fz += dz * inv_dist;
                }
            }
        }

        // 3. 突触弹簧引力场计算 (有向结构张力: 零堆分配，直取 compiled_synapses_)
        if (!is_compiled_) compile();
        for (size_t k = 0; k < compiled_synapses_.size(); ++k) {
            const auto& syn = compiled_synapses_[k];
            if (syn.from_idx >= cells.size() || syn.to_idx >= cells.size()) continue;
            auto& c1 = cells[syn.from_idx];
            auto& c2 = cells[syn.to_idx];

            float dx = c2.x - c1.x;
            float dy = c2.y - c1.y;
            float dz = c2.z - c1.z;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz + 1e-4f);

            float delta = dist - 60.0f;
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
        }

        for (auto& syn : synapses) {
            // 推进神经放电光子动画
            if (syn.photon_pos >= 0.0f) {
                syn.photon_pos += dt * 3.0f;
                if (syn.photon_pos > 1.0f) syn.photon_pos = -1.0f;
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
        double positive_action{0.0}; // 买入/加速/左偏
        double negative_action{0.0}; // 卖出/制动/右偏
        double defensive_reset{0.0}; // 清仓/居中
        bool   immune_lock{false};   // 免疫熔断

        // === 预测编码与概念吸引子输出 (Predictive World Model & Thought Dynamics) ===
        double predicted_sense_0{0.0}; // 内部对输入0的前瞻预测
        double predicted_sense_1{0.0}; // 内部对输入1的前瞻预测
        double prediction_error{0.0};  // 预测惊奇度 (Surprise)
        double thought_energy{0.0};    // 内部神经节相空间总动能
        const char* thought_mode{"STABLE_ATTRACTOR"}; // "EXPLORATION", "FOCUS", "SURPRISE", "STABLE_ATTRACTOR"
    };

    ActionOutputs forward(const double inputs[4], bool enable_hebbian = true) {
        if (!is_compiled_) compile();

        // 1. 清空扁平输入端口缓冲 (连续内存快速清零)
        double* __restrict port_ptr = flat_port_inputs_.data();
        std::memset(port_ptr, 0, flat_port_inputs_.size() * sizeof(double));

        Cell* __restrict cells_ptr = cells.data();
        auto* __restrict syn_ptr = compiled_synapses_.data();
        const size_t num_synapses = compiled_synapses_.size();

        // 2. 注入时序循环反馈信号 (一等公民 Recurrent Loops: 取上一时间步记忆状态 prev_output_val)
        for (size_t i = 0; i < num_synapses; ++i) {
            const auto& syn = syn_ptr[i];
            if (syn.is_recurrent) {
                double val = cells_ptr[syn.from_idx].prev_output_val * syn.weight;
                port_ptr[syn.to_idx * 2 + syn.to_port] += val;
            }
        }

        // 3. 按预编译拓扑顺序逐一激发细胞并即时前向传播
        const auto* __restrict order_ptr = execution_order_.data();
        const size_t num_ordered = execution_order_.size();

        for (size_t i = 0; i < num_ordered; ++i) {
            size_t idx = order_ptr[i];
            auto& c = cells_ptr[idx];
            double in0 = port_ptr[idx * 2 + 0];
            double in1 = port_ptr[idx * 2 + 1];

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
                case CellType::OP_DELAY_N: {
                    int k = std::clamp(static_cast<int>(std::floor(c.param1 * 16.0)), 1, 16);
                    size_t read_idx = (static_cast<size_t>(c.delay_idx) + 16 - static_cast<size_t>(k)) & 15;
                    c.output_val = c.delay_buffer[read_idx];
                    c.delay_buffer[c.delay_idx & 15] = in0;
                    c.delay_idx = static_cast<uint8_t>((c.delay_idx + 1) & 15);
                    break;
                }
                case CellType::OP_OSCILLATOR: {
                    if (c.activation_count == 0 && std::abs(c.state_val) < 1e-6 && std::abs(c.aux_state) < 1e-6) {
                        c.state_val = 0.1;
                    }
                    double mu = (std::abs(c.param1) > 1e-4) ? std::clamp(std::abs(c.param1), 0.01, 5.0) : 1.0;
                    double dt = (std::abs(c.param2) > 1e-4) ? std::clamp(std::abs(c.param2), 0.001, 0.2) : 0.05;

                    double s1 = c.state_val;
                    double s2 = c.aux_state;

                    double ds1 = s2;
                    double ds2 = mu * (1.0 - s1 * s1) * s2 - s1 + in0;

                    s1 += ds1 * dt;
                    s2 += ds2 * dt;

                    c.state_val = std::clamp(s1, -10.0, 10.0);
                    c.aux_state = std::clamp(s2, -10.0, 10.0);
                    c.output_val = c.state_val;
                    break;
                }
                case CellType::OP_QUADRATIC:
                    c.output_val = c.param1 * in0 * in0 + c.param2 * in0 * in1;
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
                case CellType::GATE_DEADZONE:
                    c.output_val = (std::abs(in0) > std::abs(c.param1)) ? in0 : 0.0;
                    break;
                case CellType::GATE_MIN_MAX:
                    c.output_val = (c.param1 > 0.5) ? std::max(in0, in1) : std::min(in0, in1);
                    break;

                case CellType::ACT_PRIMARY_POSITIVE:
                case CellType::ACT_PRIMARY_NEGATIVE:
                case CellType::ACT_DEFENSIVE_RESET:
                case CellType::ACT_IMMUNE_BLOCK:
                case CellType::PREDICT_SENSE_0:
                case CellType::PREDICT_SENSE_1:
                    c.output_val = in0;
                    break;

                case CellType::ASSOCIATION_HUB:
                    c.output_val = std::tanh(in0 + in1 * c.param1);
                    break;
            }

            if (std::abs(c.output_val) > 1e-6) {
                c.activation_count++;
                c.glow_charge = std::min(1.0f, c.glow_charge + 0.3f);
            }

            // 前向突触即时传递至后续位阶节点
            for (size_t s = 0; s < num_synapses; ++s) {
                const auto& syn = syn_ptr[s];
                if (!syn.is_recurrent && syn.from_idx == idx) {
                    port_ptr[syn.to_idx * 2 + syn.to_port] += c.output_val * syn.weight;
                }
            }
        }

        // 4. 终身在线 Oja 赫布可塑性学习 (Lifelong Hebbian Plasticity In-Place Update)
        if (enable_hebbian) {
            for (size_t i = 0; i < num_synapses; ++i) {
                auto& syn = syn_ptr[i];
                if (syn.hebbian_rate <= 1e-6) continue;

                double u_pre = cells_ptr[syn.from_idx].output_val;
                double u_post = cells_ptr[syn.to_idx].output_val;

                // Oja 规则: delta_w = eta * (u_pre * u_post - decay * u_post^2 * w)
                double delta_w = syn.hebbian_rate * (u_pre * u_post - syn.hebbian_decay * u_post * u_post * syn.weight);
                syn.weight = std::clamp(syn.weight + delta_w, -3.0, 3.0);
                if (!std::isfinite(syn.weight)) syn.weight = syn.initial_weight;
            }
        }

        // 5. 提交当前时刻输出至 prev_output_val 作为下一时刻循环记忆
        for (size_t i = 0; i < cells.size(); ++i) {
            cells_ptr[i].prev_output_val = cells_ptr[i].output_val;
        }

        // 6. 收集最终动作信号与概念相空间状态
        ActionOutputs actions{};
        double total_energy = 0.0;
        for (const auto& c : cells) {
            total_energy += c.output_val * c.output_val;
        }
        actions.thought_energy = total_energy;

        for (const auto& ac : compiled_actions_) {
            double val = cells_ptr[ac.cell_idx].output_val;
            if (ac.type == CellType::ACT_PRIMARY_POSITIVE) actions.positive_action = val;
            else if (ac.type == CellType::ACT_PRIMARY_NEGATIVE) actions.negative_action = val;
            else if (ac.type == CellType::ACT_DEFENSIVE_RESET) actions.defensive_reset = val;
            else if (ac.type == CellType::ACT_IMMUNE_BLOCK && val > 0.5) actions.immune_lock = true;
            else if (ac.type == CellType::PREDICT_SENSE_0) actions.predicted_sense_0 = val;
            else if (ac.type == CellType::PREDICT_SENSE_1) actions.predicted_sense_1 = val;
        }

        // 预测误差 (Surprise)
        double err0 = inputs[0] - actions.predicted_sense_0;
        double err1 = inputs[1] - actions.predicted_sense_1;
        actions.prediction_error = std::sqrt(err0 * err0 + err1 * err1);

        if (actions.prediction_error > 5.0) actions.thought_mode = "SURPRISE";
        else if (actions.thought_energy > 8.0) actions.thought_mode = "FOCUS";
        else if (actions.thought_energy < 0.2) actions.thought_mode = "EXPLORATION";
        else actions.thought_mode = "STABLE_ATTRACTOR";

        return actions;
    }

    // ── 闭门心理推演与反事实想象 (Mental Simulation / Thought Rollout) ──
    // 切断真实外部输入，网络利用自身预测受体与时序递归环路在几微秒内内部试错自激推演未来轨迹！
    std::vector<ActionOutputs> simulate_mental_rollout(int rollout_steps = 5) {
        std::vector<ActionOutputs> imagined_trajectory;
        imagined_trajectory.reserve(rollout_steps);

        double simulated_inputs[4] = {
            cells.empty() ? 0.0 : cells[0].output_val,
            cells.size() > 1 ? cells[1].output_val : 0.0,
            0.0, 0.0
        };

        for (int step = 0; step < rollout_steps; ++step) {
            auto act = forward(simulated_inputs, false); // 心理推演中不污染真实外部突触
            imagined_trajectory.push_back(act);

            // 自回输：将内部预测作为下一步推演的世界模型输入
            simulated_inputs[0] = act.predicted_sense_0;
            simulated_inputs[1] = act.predicted_sense_1;
        }

        return imagined_trajectory;
    }

    // 导出全息 3D 可视化 JSON 数据帧 (包含物理坐标、发光电位、循环连接、突触可塑性与思维相空间动能)
    std::string to_json() const {
        std::ostringstream ss;
        double safe_fitness = std::isfinite(fitness_score) ? fitness_score : 0.0;
        double total_e = 0.0;
        for (const auto& c : cells) total_e += c.output_val * c.output_val;

        ss << "{\n";
        ss << "  \"organism_id\": " << organism_id << ",\n";
        ss << "  \"generation\": " << generation << ",\n";
        ss << "  \"lineage\": \"" << lineage_name << "\",\n";
        ss << "  \"fitness\": " << safe_fitness << ",\n";
        ss << "  \"thought_energy\": " << total_e << ",\n";
        ss << "  \"cells\": [\n";
        for (size_t i = 0; i < cells.size(); ++i) {
            const auto& c = cells[i];
            double safe_out = std::isfinite(c.output_val) ? c.output_val : 0.0;
            float safe_x = std::isfinite(c.x) ? c.x : 0.0f;
            float safe_y = std::isfinite(c.y) ? c.y : 0.0f;
            float safe_z = std::isfinite(c.z) ? c.z : 0.0f;
            float safe_glow = std::isfinite(c.glow_charge) ? c.glow_charge : 0.0f;
            ss << "    {\"id\": " << c.id << ", \"type\": \"" << to_string(c.type) << "\", "
               << "\"param1\": " << (std::isfinite(c.param1) ? c.param1 : 0.0) << ", "
               << "\"param2\": " << (std::isfinite(c.param2) ? c.param2 : 0.0) << ", "
               << "\"output\": " << safe_out << ", \"activations\": " << c.activation_count << ", "
               << "\"x\": " << safe_x << ", \"y\": " << safe_y << ", \"z\": " << safe_z << ", "
               << "\"glow\": " << safe_glow << "}"
               << (i + 1 < cells.size() ? "," : "") << "\n";
        }
        ss << "  ],\n";
        ss << "  \"synapses\": [\n";
        for (size_t i = 0; i < synapses.size(); ++i) {
            const auto& s = synapses[i];
            double safe_w = std::isfinite(s.weight) ? s.weight : 0.0;
            float safe_p = std::isfinite(s.photon_pos) ? s.photon_pos : 0.0f;
            ss << "    {\"from\": " << s.from_cell_id << ", \"to\": " << s.to_cell_id << ", "
               << "\"port\": " << (int)s.to_port << ", \"weight\": " << safe_w << ", "
               << "\"active\": " << (s.is_active ? "true" : "false") << ", "
               << "\"recurrent\": " << (s.is_recurrent ? "true" : "false") << ", "
               << "\"hebb_rate\": " << s.hebbian_rate << ", "
               << "\"photon_pos\": " << safe_p << "}"
               << (i + 1 < synapses.size() ? "," : "") << "\n";
        }
        ss << "  ]\n";
        ss << "}";
        return ss.str();
    }

    // ── 鲍德温效应与基因固化 (Baldwin Effect & Genetic Crystallization) ──
    // 将个体后天生命期内通过 Oja 在线学习到的有效突触权重，固化并写入为遗传初始基线！
    void crystallize_plasticity() {
        for (auto& syn : synapses) {
            syn.initial_weight = syn.weight;
        }
        for (auto& csyn : compiled_synapses_) {
            csyn.initial_weight = csyn.weight;
        }
    }

    // ── 全息检查点反序列化 (JSON Deserialization) ──
    static CellularOrganism from_json(const std::string& json_str) {
        CellularOrganism org;

        auto find_val = [&](const std::string& key) -> std::string {
            auto pos = json_str.find("\"" + key + "\"");
            if (pos == std::string::npos) return "";
            auto colon = json_str.find(":", pos);
            if (colon == std::string::npos) return "";
            size_t start = colon + 1;
            while (start < json_str.size() && (json_str[start] == ' ' || json_str[start] == '\t' || json_str[start] == '\n' || json_str[start] == '\r')) start++;
            if (start >= json_str.size()) return "";
            if (json_str[start] == '\"') {
                size_t end = json_str.find('\"', start + 1);
                if (end == std::string::npos) return "";
                return json_str.substr(start + 1, end - start - 1);
            } else {
                size_t end = start;
                while (end < json_str.size() && json_str[end] != ',' && json_str[end] != '}' && json_str[end] != '\n' && json_str[end] != '\r') end++;
                return json_str.substr(start, end - start);
            }
        };

        std::string id_str = find_val("organism_id");
        if (!id_str.empty()) org.organism_id = static_cast<uint64_t>(std::stoull(id_str));
        std::string gen_str = find_val("generation");
        if (!gen_str.empty()) org.generation = static_cast<uint32_t>(std::stoul(gen_str));
        std::string lin_str = find_val("lineage");
        if (!lin_str.empty()) org.lineage_name = lin_str;
        std::string fit_str = find_val("fitness");
        if (!fit_str.empty()) org.fitness_score = std::stod(fit_str);

        // 解析 cells
        auto cells_pos = json_str.find("\"cells\"");
        if (cells_pos != std::string::npos) {
            auto open_bracket = json_str.find("[", cells_pos);
            auto close_bracket = json_str.find("]", open_bracket);
            if (open_bracket != std::string::npos && close_bracket != std::string::npos) {
                std::string cells_block = json_str.substr(open_bracket, close_bracket - open_bracket + 1);
                size_t pos = 0;
                while ((pos = cells_block.find("{", pos)) != std::string::npos) {
                    size_t end_brace = cells_block.find("}", pos);
                    if (end_brace == std::string::npos) break;
                    std::string cell_item = cells_block.substr(pos, end_brace - pos + 1);

                    auto parse_field = [&](const std::string& k) -> std::string {
                        auto p = cell_item.find("\"" + k + "\"");
                        if (p == std::string::npos) return "";
                        auto c = cell_item.find(":", p);
                        if (c == std::string::npos) return "";
                        size_t s = c + 1;
                        while (s < cell_item.size() && (cell_item[s] == ' ' || cell_item[s] == '\t')) s++;
                        if (s >= cell_item.size()) return "";
                        if (cell_item[s] == '\"') {
                            size_t e = cell_item.find('\"', s + 1);
                            if (e == std::string::npos) return "";
                            return cell_item.substr(s + 1, e - s - 1);
                        } else {
                            size_t e = s;
                            while (e < cell_item.size() && cell_item[e] != ',' && cell_item[e] != '}') e++;
                            return cell_item.substr(s, e - s);
                        }
                    };

                    uint16_t cid = static_cast<uint16_t>(std::stoul(parse_field("id").empty() ? "0" : parse_field("id")));
                    CellType ctype = cell_type_from_string(parse_field("type"));
                    double p1 = parse_field("param1").empty() ? 0.0 : std::stod(parse_field("param1"));
                    double p2 = parse_field("param2").empty() ? 0.0 : std::stod(parse_field("param2"));
                    float x = parse_field("x").empty() ? 0.0f : std::stof(parse_field("x"));
                    float y = parse_field("y").empty() ? 0.0f : std::stof(parse_field("y"));
                    float z = parse_field("z").empty() ? 0.0f : std::stof(parse_field("z"));

                    Cell c{cid, ctype, p1, p2, 0.0, 0.0, false, 0.0, 0, 0, x, y, z};
                    org.cells.push_back(c);
                    pos = end_brace + 1;
                }
            }
        }

        // 解析 synapses
        auto syn_pos = json_str.find("\"synapses\"");
        if (syn_pos != std::string::npos) {
            auto open_bracket = json_str.find("[", syn_pos);
            auto close_bracket = json_str.find("]", open_bracket);
            if (open_bracket != std::string::npos && close_bracket != std::string::npos) {
                std::string syn_block = json_str.substr(open_bracket, close_bracket - open_bracket + 1);
                size_t pos = 0;
                while ((pos = syn_block.find("{", pos)) != std::string::npos) {
                    size_t end_brace = syn_block.find("}", pos);
                    if (end_brace == std::string::npos) break;
                    std::string syn_item = syn_block.substr(pos, end_brace - pos + 1);

                    auto parse_field = [&](const std::string& k) -> std::string {
                        auto p = syn_item.find("\"" + k + "\"");
                        if (p == std::string::npos) return "";
                        auto c = syn_item.find(":", p);
                        if (c == std::string::npos) return "";
                        size_t s = c + 1;
                        while (s < syn_item.size() && (syn_item[s] == ' ' || syn_item[s] == '\t')) s++;
                        if (s >= syn_item.size()) return "";
                        if (syn_item[s] == '\"') {
                            size_t e = syn_item.find('\"', s + 1);
                            if (e == std::string::npos) return "";
                            return syn_item.substr(s + 1, e - s - 1);
                        } else {
                            size_t e = s;
                            while (e < syn_item.size() && syn_item[e] != ',' && syn_item[e] != '}') e++;
                            return syn_item.substr(s, e - s);
                        }
                    };

                    uint16_t from = static_cast<uint16_t>(std::stoul(parse_field("from").empty() ? "0" : parse_field("from")));
                    uint16_t to = static_cast<uint16_t>(std::stoul(parse_field("to").empty() ? "0" : parse_field("to")));
                    uint8_t port = static_cast<uint8_t>(std::stoul(parse_field("port").empty() ? "0" : parse_field("port")));
                    double weight = parse_field("weight").empty() ? 1.0 : std::stod(parse_field("weight"));
                    bool active = (parse_field("active") == "true");
                    double hebb_rate = parse_field("hebb_rate").empty() ? 0.005 : std::stod(parse_field("hebb_rate"));

                    Synapse s{from, to, port, weight, active, 60.0f, -1.0f};
                    s.initial_weight = weight;
                    s.hebbian_rate = hebb_rate;
                    s.hebbian_decay = 0.02;
                    org.synapses.push_back(s);
                    pos = end_brace + 1;
                }
            }
        }

        org.compile();
        return org;
    }

    bool save_checkpoint_json(const std::string& filepath) const {
        std::ofstream ofs(filepath);
        if (!ofs.is_open()) return false;
        ofs << to_json();
        return true;
    }

    static CellularOrganism load_checkpoint_json(const std::string& filepath) {
        std::ifstream ifs(filepath);
        if (!ifs.is_open()) return CellularOrganism{};
        std::stringstream buffer;
        buffer << ifs.rdbuf();
        return from_json(buffer.str());
    }
};

// ============================================================================
// 6. 形态发生进化引擎 (MorphogeneticEvolutionEngine)
// ============================================================================
class MorphogeneticEvolutionEngine {
public:
    // 行为特征空间新颖性档案 (Novelty Archive for Intrinsic Motivation / Open-Ended Curiosity)
    struct NoveltyArchive {
        std::vector<std::vector<double>> archive;
        size_t max_archive_size{200};
        size_t k_neighbors{5};

        double compute_novelty(const std::vector<double>& behavior_vec) const {
            if (archive.empty() || behavior_vec.empty()) return 1.0;
            std::vector<double> distances;
            distances.reserve(archive.size());
            for (const auto& a : archive) {
                double d2 = 0.0;
                size_t dim = std::min(behavior_vec.size(), a.size());
                for (size_t i = 0; i < dim; ++i) {
                    double diff = behavior_vec[i] - a[i];
                    d2 += diff * diff;
                }
                distances.push_back(std::sqrt(d2));
            }
            std::sort(distances.begin(), distances.end());
            size_t k = std::min(k_neighbors, distances.size());
            if (k == 0) return 1.0;
            double sum_dist = 0.0;
            for (size_t i = 0; i < k; ++i) sum_dist += distances[i];
            return sum_dist / static_cast<double>(k);
        }

        void add_behavior(const std::vector<double>& b) {
            if (b.empty()) return;
            archive.push_back(b);
            if (archive.size() > max_archive_size) {
                archive.erase(archive.begin());
            }
        }
    };

    explicit MorphogeneticEvolutionEngine(size_t population_size = 20, uint32_t seed = 42, SeedInitMode init_mode = SeedInitMode::HANDCRAFTED_PROGENITOR)
        : rng_(seed), population_size_(population_size) {
        constraint_cfg_.seed_mode = init_mode;
        init_population();
    }

    explicit MorphogeneticEvolutionEngine(size_t population_size, uint32_t seed, const EvolutionConstraintConfig& cfg)
        : rng_(seed), population_size_(population_size), constraint_cfg_(cfg) {
        init_population();
    }

    void init_population() {
        population_.clear();
        for (size_t i = 0; i < population_size_; ++i) {
            auto org = CellularOrganism::create_by_mode(constraint_cfg_.seed_mode, i + 1, static_cast<uint32_t>(rng_()));
            if (i > 0) {
                for (int m = 0; m < 3; ++m) mutate(org);
            }
            population_.push_back(std::move(org));
        }
    }

    void set_init_mode(SeedInitMode mode) {
        constraint_cfg_.seed_mode = mode;
        init_population();
    }

    void set_constraint_config(const EvolutionConstraintConfig& cfg) {
        constraint_cfg_ = cfg;
        init_population();
    }

    const EvolutionConstraintConfig& get_constraint_config() const { return constraint_cfg_; }
    EvolutionConstraintConfig& constraint_config() { return constraint_cfg_; }
    SeedInitMode get_init_mode() const { return constraint_cfg_.seed_mode; }

    // ── 生物变异操作 1: 细胞分裂增殖 (Mitosis / Add Cell) ──
    bool mutate_add_cell(CellularOrganism& org) {
        if (org.synapses.empty()) return mutate_add_synapse(org);
        std::uniform_int_distribution<size_t> dist_syn(0, org.synapses.size() - 1);
        size_t s_idx = dist_syn(rng_);
        auto& old_syn = org.synapses[s_idx];
        if (!old_syn.is_active) return false;

        static const CellType curated_9_candidates[] = {
            CellType::OP_EMA, CellType::OP_DIFF, CellType::OP_INTEGRAL,
            CellType::OP_SUM, CellType::OP_SUB, CellType::OP_MULTIPLY,
            CellType::OP_RATIO, CellType::OP_ABS,
            CellType::GATE_HYSTERESIS
        };

        static const CellType full_24_candidates[] = {
            CellType::OP_EMA, CellType::OP_DIFF, CellType::OP_INTEGRAL,
            CellType::OP_SUM, CellType::OP_SUB, CellType::OP_MULTIPLY,
            CellType::OP_RATIO, CellType::OP_ABS,
            CellType::OP_DELAY_N, CellType::OP_OSCILLATOR, CellType::OP_QUADRATIC,
            CellType::GATE_THRESHOLD, CellType::GATE_HYSTERESIS,
            CellType::GATE_AND, CellType::GATE_INHIBIT, CellType::GATE_DEADZONE, CellType::GATE_MIN_MAX,
            CellType::PREDICT_SENSE_0, CellType::PREDICT_SENSE_1, CellType::ASSOCIATION_HUB
        };

        CellType new_type;
        if (constraint_cfg_.type_whitelist == TypeWhitelistMode::CURATED_9) {
            std::uniform_int_distribution<size_t> dist_type(0, sizeof(curated_9_candidates) / sizeof(curated_9_candidates[0]) - 1);
            new_type = curated_9_candidates[dist_type(rng_)];
        } else {
            std::uniform_int_distribution<size_t> dist_type(0, sizeof(full_24_candidates) / sizeof(full_24_candidates[0]) - 1);
            new_type = full_24_candidates[dist_type(rng_)];
        }

        // 解除骨架锁 (UNLOCKED) 约束下，允许演化出额外感受受体或效应器动作神经元
        if (constraint_cfg_.skeleton_lock == SkeletonLockMode::UNLOCKED) {
            std::uniform_real_distribution<double> dist_skel(0.0, 1.0);
            if (dist_skel(rng_) < 0.15) {
                static const CellType skel_candidates[] = {
                    CellType::SENSE_RAW_INPUT_0, CellType::SENSE_RAW_INPUT_1,
                    CellType::SENSE_RAW_INPUT_2, CellType::SENSE_RAW_INPUT_3,
                    CellType::ACT_PRIMARY_POSITIVE, CellType::ACT_PRIMARY_NEGATIVE,
                    CellType::ACT_DEFENSIVE_RESET, CellType::ACT_IMMUNE_BLOCK
                };
                std::uniform_int_distribution<size_t> dist_sk(0, sizeof(skel_candidates) / sizeof(skel_candidates[0]) - 1);
                new_type = skel_candidates[dist_sk(rng_)];
            }
        }

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

        Synapse syn1{from_id, new_id, 0, 1.0, true, 60.0f, -1.0f};
        syn1.initial_weight = 1.0;
        syn1.hebbian_rate = 0.005;
        syn1.hebbian_decay = 0.02;

        Synapse syn2{new_id, to_id, to_port, orig_weight, true, 60.0f, -1.0f};
        syn2.initial_weight = orig_weight;
        syn2.hebbian_rate = 0.005;
        syn2.hebbian_decay = 0.02;

        org.synapses.push_back(syn1);
        org.synapses.push_back(syn2);

        org.compile();
        return true;
    }

    // ── 生物变异操作 2: 突触跨界重连 (Synaptic Rewiring & Morphogenetic Spatial Wiring) ──
    bool mutate_add_synapse(CellularOrganism& org) {
        if (org.cells.size() < 2) return false;
        for (int retry = 0; retry < 12; ++retry) {
            std::uniform_int_distribution<size_t> dist_cell(0, org.cells.size() - 1);
            size_t idx_a = dist_cell(rng_);
            size_t idx_b = dist_cell(rng_);
            if (idx_a == idx_b) continue;

            if (org.cells[idx_b].type == CellType::SENSE_RAW_INPUT_0 ||
                org.cells[idx_b].type == CellType::SENSE_RAW_INPUT_1 ||
                org.cells[idx_b].type == CellType::SENSE_RAW_INPUT_2 ||
                org.cells[idx_b].type == CellType::SENSE_RAW_INPUT_3) {
                continue;
            }

            // 空间场发育指引: 3D 物理空间欧氏距离越近，成键概率越高 (自发形成小世界皮层柱)
            float dx = org.cells[idx_b].x - org.cells[idx_a].x;
            float dy = org.cells[idx_b].y - org.cells[idx_a].y;
            float dz = org.cells[idx_b].z - org.cells[idx_a].z;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            double conn_prob = std::exp(-dist / 80.0f) + 0.15;
            std::uniform_real_distribution<double> dist_p(0.0, 1.0);
            if (dist_p(rng_) > conn_prob && retry < 8) continue;

            uint16_t from_id = org.cells[idx_a].id;
            uint16_t to_id = org.cells[idx_b].id;

            std::uniform_real_distribution<double> dist_weight(-2.0, 2.0);
            std::uniform_int_distribution<int> dist_port(0, 1);

            Synapse new_syn{from_id, to_id, static_cast<uint8_t>(dist_port(rng_)), dist_weight(rng_), true, 60.0f, -1.0f};
            new_syn.initial_weight = new_syn.weight;
            new_syn.hebbian_rate = 0.005;
            new_syn.hebbian_decay = 0.02;

            org.synapses.push_back(new_syn);
            org.compile();
            return true;
        }
        return false;
    }

    // ── 生物变异操作 3: 细胞参数微调 + 突触可塑性 (Metabolic Drift & Plasticity) ──
    bool mutate_parameters(CellularOrganism& org) {
        if (org.cells.empty()) return false;
        std::uniform_int_distribution<size_t> dist_cell(0, org.cells.size() - 1);
        auto& c = org.cells[dist_cell(rng_)];

        std::normal_distribution<double> dist_noise(0.0, 0.05);
        c.param1 = std::clamp(c.param1 + dist_noise(rng_), -5.0, 5.0);
        c.param2 = std::clamp(c.param2 + dist_noise(rng_), -5.0, 5.0);

        if (!org.synapses.empty()) {
            std::uniform_int_distribution<size_t> dist_syn(0, org.synapses.size() - 1);
            auto& syn = org.synapses[dist_syn(rng_)];
            std::normal_distribution<double> dist_w(0.0, 0.15);
            syn.weight = std::clamp(syn.weight + dist_w(rng_), -3.0, 3.0);
            syn.initial_weight = syn.weight;

            // 突触学习率微调漂移 (慢时标系统发育演化)
            std::normal_distribution<double> dist_h(0.0, 0.002);
            syn.hebbian_rate = std::clamp(syn.hebbian_rate + dist_h(rng_), 0.0, 0.05);

            org.compile();
        }
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

        if (constraint_cfg_.skeleton_lock == SkeletonLockMode::LOCKED) {
            // 受限骨架模式：受体永远免疫凋亡
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
        } else {
            // 自由形态发生模式：无用受体亦可凋亡
            org.cells.erase(
                std::remove_if(org.cells.begin(), org.cells.end(), [&](const Cell& c) {
                    return !useful_cells.count(c.id);
                }),
                org.cells.end()
            );
        }

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

        // 1. 若当前有机体突触不足 2 条 (如原始胚胎冷启动)，强制优先建立基础突触连接
        size_t active_syns = 0;
        for (const auto& s : org.synapses) if (s.is_active) active_syns++;
        if (active_syns < 2) {
            mutate_add_synapse(org);
        }

        // 2. 突触权重与细胞参数微调 (细粒度梯度微扰，随停滞自适应增强)
        if (dist(rng_) < std::min(0.95, 0.80 * adaptive_mutation_boost_)) {
            mutate_parameters(org);
        }

        // 3. 突触结构重连 / 新增突触
        if (dist(rng_) < std::min(0.85, 0.45 * adaptive_mutation_boost_)) {
            mutate_add_synapse(org);
        }

        // 4. 细胞分裂增殖 (插入代谢/门控算子神经元)
        if (dist(rng_) < std::min(0.70, 0.30 * adaptive_mutation_boost_)) {
            mutate_add_cell(org);
        }

        // 5. 细胞自我凋亡与无用分支净化
        if (dist(rng_) < 0.05) {
            prune_apoptosis(org);
        }
    }

    // ── 种群世代演化 (Evolve Next Generation) ──
    void evolve_generation() {
        // 若启用了新颖性好奇心驱动，综合内在与外在动机评分
        if (constraint_cfg_.fitness_driver == FitnessDriverMode::NOVELTY_SEARCH ||
            constraint_cfg_.fitness_driver == FitnessDriverMode::HYBRID_CURIOSITY) {
            for (auto& org : population_) {
                std::vector<double> b_vec = {
                    static_cast<double>(org.cells.size()),
                    static_cast<double>(org.synapses.size()),
                    org.total_pnl,
                    org.fitness_score
                };
                double nov = novelty_archive_.compute_novelty(b_vec);
                novelty_archive_.add_behavior(b_vec);

                if (constraint_cfg_.fitness_driver == FitnessDriverMode::NOVELTY_SEARCH) {
                    org.fitness_score = nov * 100.0;
                } else if (constraint_cfg_.fitness_driver == FitnessDriverMode::HYBRID_CURIOSITY) {
                    double alpha = constraint_cfg_.novelty_weight;
                    org.fitness_score = (1.0 - alpha) * org.fitness_score + alpha * (nov * 50.0);
                }
            }
        }

        std::sort(population_.begin(), population_.end(), [](const CellularOrganism& a, const CellularOrganism& b) {
            return a.fitness_score > b.fitness_score;
        });

        // 自适应停滞检测 (Plateau Stagnation Detection)
        if (!population_.empty()) {
            double current_best = population_[0].fitness_score;
            if (current_best > best_historical_fitness_ + 1e-4) {
                best_historical_fitness_ = current_best;
                stagnation_generations_ = 0;
                adaptive_mutation_boost_ = 1.0;
            } else {
                stagnation_generations_++;
                if (stagnation_generations_ > 15) {
                    // 陷入停滞高原: 激变模式 (Hyper-mutation burst 动态加温)
                    adaptive_mutation_boost_ = std::min(2.5, 1.0 + (stagnation_generations_ - 15) * 0.08);
                }
            }
        }

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
    std::vector<CellularOrganism>& population() { return population_; }
    uint32_t get_stagnation_generations() const { return stagnation_generations_; }
    double get_adaptive_mutation_boost() const { return adaptive_mutation_boost_; }
    NoveltyArchive& novelty_archive() { return novelty_archive_; }

    // ── 种群级全息检查点保存 (Population Checkpoint Save) ──
    bool save_population_checkpoint(const std::string& filepath) const {
        std::ofstream ofs(filepath);
        if (!ofs.is_open()) return false;
        ofs << "{\n";
        ofs << "  \"population_size\": " << population_.size() << ",\n";
        ofs << "  \"stagnation_generations\": " << stagnation_generations_ << ",\n";
        ofs << "  \"best_historical_fitness\": " << best_historical_fitness_ << ",\n";
        ofs << "  \"organisms\": [\n";
        for (size_t i = 0; i < population_.size(); ++i) {
            ofs << population_[i].to_json() << (i + 1 < population_.size() ? ",\n" : "\n");
        }
        ofs << "  ]\n";
        ofs << "}\n";
        return true;
    }

    // ── 种群级全息检查点加载与断点续演化 (Population Checkpoint Load & Warm Resume) ──
    bool load_population_checkpoint(const std::string& filepath) {
        std::ifstream ifs(filepath);
        if (!ifs.is_open()) return false;
        std::stringstream buffer;
        buffer << ifs.rdbuf();
        std::string content = buffer.str();

        auto orgs_pos = content.find("\"organisms\"");
        if (orgs_pos == std::string::npos) return false;

        auto open_bracket = content.find("[", orgs_pos);
        auto close_bracket = content.rfind("]");
        if (open_bracket == std::string::npos || close_bracket == std::string::npos) return false;

        std::string list_str = content.substr(open_bracket + 1, close_bracket - open_bracket - 1);
        std::vector<CellularOrganism> loaded_pop;

        size_t depth = 0;
        size_t start = 0;
        bool in_obj = false;

        for (size_t i = 0; i < list_str.size(); ++i) {
            if (list_str[i] == '{') {
                if (depth == 0) { start = i; in_obj = true; }
                depth++;
            } else if (list_str[i] == '}') {
                depth--;
                if (depth == 0 && in_obj) {
                    std::string obj_str = list_str.substr(start, i - start + 1);
                    auto org = CellularOrganism::from_json(obj_str);
                    if (!org.cells.empty()) loaded_pop.push_back(std::move(org));
                    in_obj = false;
                }
            }
        }

        if (loaded_pop.empty()) return false;
        population_ = std::move(loaded_pop);
        population_size_ = population_.size();
        return true;
    }

private:
    std::mt19937 rng_;
    size_t population_size_{20};
    EvolutionConstraintConfig constraint_cfg_;
    std::vector<CellularOrganism> population_;
    NoveltyArchive novelty_archive_;
    uint32_t stagnation_generations_{0};
    double best_historical_fitness_{-1e9};
    double adaptive_mutation_boost_{1.0};
};

} // namespace kun
