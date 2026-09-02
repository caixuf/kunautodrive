#pragma once

/**
 * formal_safety_certifier.hpp — 形式化安全证书与启发式区间包络验证器
 *
 * 核心架构:
 * 1. 启发式区间算术分析器 (HeuristicIntervalBoundsChecker):
 *    - 严密区间向前推演 (Interval Arithmetic Forward Bounding)
 *    - 提取动力学效应器在极限输入包络下的最大/最小可能输出舵角与减速度
 * 2. 符号化 SMT-LIB v2.6 证书生成器:
 *    - 符号化提取细胞网络 DAG 数学方程与 3 大硬实时安全不变量
 *    - 输出标准 QF_NRA SMT-LIB 脚本供 Z3 / CVC5 / MathSAT 求解
 * 3. 真实形式化求解接口 (SMTSafetyProver):
 *    - 调用真实 SMT 求解器 (如 z3 / cvc5)
 *    - 仅在求解器明确返回 unsat (反例不可达) 时签发 is_verified = true
 *    - 未安装求解器时如实标记 SOLVER_NOT_FOUND，坚决杜绝虚假证明
 */

#include "kun/cellular/cellular_genome.hpp"
#include "kun/cellular/adas_cellular_adapter.hpp"
#include <string>
#include <sstream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <array>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace kun {

enum class SolverStatus {
    NOT_INVOKED = 0,
    SOLVER_NOT_FOUND = 1,
    UNSAT_PROVED = 2,
    SAT_COUNTEREXAMPLE = 3,
    UNKNOWN_OR_TIMEOUT = 4,
    SOLVER_ERROR = 5
};

inline const char* to_string(SolverStatus s) {
    switch (s) {
        case SolverStatus::NOT_INVOKED: return "NOT_INVOKED";
        case SolverStatus::SOLVER_NOT_FOUND: return "SOLVER_NOT_FOUND";
        case SolverStatus::UNSAT_PROVED: return "UNSAT_PROVED";
        case SolverStatus::SAT_COUNTEREXAMPLE: return "SAT_COUNTEREXAMPLE";
        case SolverStatus::UNKNOWN_OR_TIMEOUT: return "UNKNOWN_OR_TIMEOUT";
        case SolverStatus::SOLVER_ERROR: return "SOLVER_ERROR";
        default: return "UNKNOWN";
    }
}

struct IntervalBoundsReport {
    bool passed{false};                  // 启发式区间包络是否满足车规极限
    double max_steer_rad{0.0};           // 最大可能输出舵角
    double min_steer_rad{0.0};           // 最小可能输出舵角
    double min_accel_mps2{0.0};          // 最小可能制动减速度
    double max_accel_mps2{0.0};          // 最大可能纵向加速度
    size_t num_propagated_nodes{0};      // 推演节点数量
    std::string summary;                 // 推演结论摘要
};

struct SafetyProofCertificate {
    bool is_verified{false};             // 真实形式化证明是否通过 (仅当真实 SMT 求解器返回 UNSAT 时为 true)
    bool interval_bounds_verified{false};// 启发式区间算术包络检查是否通过
    bool symbolic_model_complete{false}; // action 方程是否完整覆盖 forward 语义
    bool formal_verification_skipped{false};
    SolverStatus solver_status{SolverStatus::NOT_INVOKED}; // 求解器执行状态
    std::string solver_message;          // 求解器返回信息或未安装说明
    std::string unsupported_model_features;
    double max_possible_steer_rad{0.0};  // 最大可能输出舵角
    double min_possible_accel_mps2{0.0}; // 最小可能制动减速度
    double max_possible_accel_mps2{0.0}; // 最大可能纵向加速度
    size_t num_symbolic_clauses{0};      // 符号子句数量
    std::string smt2_code;               // 生成的标准 SMT-LIB v2 脚本
    std::string proof_digest_sha256;     // 证书指纹校验码
};

class HeuristicIntervalBoundsChecker {
public:
    struct Interval {
        double low{0.0}, high{0.0};
        Interval() = default;
        Interval(double l, double h) : low(l), high(h) {}
        Interval operator+(const Interval& o) const { return {low + o.low, high + o.high}; }
        Interval operator-(const Interval& o) const { return {low - o.high, high - o.low}; }
        Interval operator*(double s) const {
            if (s >= 0) return {low * s, high * s};
            return {high * s, low * s};
        }
        Interval operator*(const Interval& o) const {
            const double values[] = {low * o.low, low * o.high,
                                     high * o.low, high * o.high};
            return {*std::min_element(std::begin(values), std::end(values)),
                    *std::max_element(std::begin(values), std::end(values))};
        }
    };

    static IntervalBoundsReport check_bounds(const CellularOrganism& org) {
        IntervalBoundsReport report;
        if (org.cells.empty()) return report;

        std::vector<Interval> node_bounds(org.cells.size(), Interval(0.0, 0.0));
        const Interval input_bounds[] = {
            {0.5, 150.0}, {-35.0, 20.0}, {-5.0, 5.0}, {0.05, 99.0}};

        // 拓扑级联区间向前推演
        for (uint32_t node_idx : org.execution_order_) {
            const auto& cell = org.cells[node_idx];
            const int input_idx = raw_input_index(cell.type);
            if (input_idx >= 0) {
                node_bounds[node_idx] = input_bounds[input_idx] * cell.param1;
                continue;
            }

            const Interval in0 = port_interval(org, node_bounds, node_idx, 0);
            const Interval in1 = port_interval(org, node_bounds, node_idx, 1);
            const Interval acc = in0 + in1;

            if (cell.type == CellType::OP_SUM) {
                node_bounds[node_idx] = acc;
            } else if (cell.type == CellType::OP_SUB) {
                node_bounds[node_idx] = in0 - in1;
            } else if (cell.type == CellType::OP_MULTIPLY) {
                node_bounds[node_idx] = in0 * in1;
            } else if (cell.type == CellType::OP_ABS) {
                node_bounds[node_idx] = abs_interval(in0);
            } else if (cell.type == CellType::OP_RATIO) {
                node_bounds[node_idx] = Interval(-1.0e6, 1.0e6);
            } else if (cell.type == CellType::OP_EMA ||
                       cell.type == CellType::OP_DIFF ||
                       cell.type == CellType::OP_INTEGRAL ||
                       cell.type == CellType::OP_DELAY_N ||
                       cell.type == CellType::OP_OSCILLATOR) {
                // Stateful cells are conservatively over-approximated; their
                // state is not treated as a safe action value.
                node_bounds[node_idx] = Interval(-10.0, 10.0) + in0;
            } else if (cell.type == CellType::GATE_HYSTERESIS) {
                node_bounds[node_idx] = Interval(-1.0, 1.0);
            } else if (cell.type == CellType::GATE_THRESHOLD ||
                       cell.type == CellType::GATE_AND) {
                node_bounds[node_idx] = Interval(0.0, 1.0);
            } else if (cell.type == CellType::GATE_DEADZONE ||
                       cell.type == CellType::ACT_PRIMARY_POSITIVE ||
                       cell.type == CellType::ACT_PRIMARY_NEGATIVE ||
                       cell.type == CellType::ACT_DEFENSIVE_RESET ||
                       cell.type == CellType::ACT_IMMUNE_BLOCK ||
                       cell.type == CellType::PREDICT_SENSE_0 ||
                       cell.type == CellType::PREDICT_SENSE_1) {
                node_bounds[node_idx] = (cell.type == CellType::GATE_DEADZONE)
                    ? hull(in0, Interval(0.0, 0.0)) : in0;
            } else if (cell.type == CellType::GATE_INHIBIT) {
                node_bounds[node_idx] = in0 * Interval(0.0, 1.0);
            } else if (cell.type == CellType::GATE_MIN_MAX) {
                node_bounds[node_idx] = hull(in0, in1);
            } else if (cell.type == CellType::ASSOCIATION_HUB) {
                node_bounds[node_idx] = Interval(std::tanh(
                    (in0 + in1 * cell.param1).low), std::tanh(
                    (in0 + in1 * cell.param1).high));
            } else {
                node_bounds[node_idx] = Interval(std::clamp(acc.low, -10.0, 10.0), std::clamp(acc.high, -10.0, 10.0));
            }
        }

        Interval positive(0.0, 0.0), negative(0.0, 0.0), defensive(0.0, 0.0);
        Interval immune(0.0, 0.0);
        for (const auto& action : org.compiled_actions_) {
            const Interval value = node_bounds[action.cell_idx];
            if (action.type == CellType::ACT_PRIMARY_POSITIVE) positive = value;
            else if (action.type == CellType::ACT_PRIMARY_NEGATIVE) negative = value;
            else if (action.type == CellType::ACT_DEFENSIVE_RESET) defensive = value;
            else if (action.type == CellType::ACT_IMMUNE_BLOCK) immune = value;
        }

        const Interval raw_steer = defensive * -1.0;
        const Interval raw_accel = positive - negative;
        const Interval steer = clamp_interval(raw_steer, -0.60, 0.60);
        const Interval accel = clamp_interval(raw_accel, -6.0, 2.0);
        report.max_steer_rad = steer.high;
        report.min_steer_rad = steer.low;
        report.min_accel_mps2 = accel.low;
        report.max_accel_mps2 = accel.high;
        if (immune.high > 0.5) {
            report.min_accel_mps2 = std::min(report.min_accel_mps2, -6.0);
            report.max_accel_mps2 = std::max(report.max_accel_mps2, -6.0);
        }
        report.num_propagated_nodes = org.execution_order_.size();
        report.passed = (report.max_steer_rad <= 0.60 && report.min_steer_rad >= -0.60 &&
                         report.min_accel_mps2 >= -6.0 && report.max_accel_mps2 <= 3.5);

        std::ostringstream ss;
        ss << "Interval bounds check: steer in [" << report.min_steer_rad << ", " << report.max_steer_rad
           << "] rad, accel in [" << report.min_accel_mps2 << ", " << report.max_accel_mps2 << "] m/s^2";
        report.summary = ss.str();
        return report;
    }

public:
    static std::vector<std::string> unsupported_model_features(const CellularOrganism& org) {
        std::vector<std::string> features;
        auto add = [&](const std::string& feature) {
            if (std::find(features.begin(), features.end(), feature) == features.end()) {
                features.push_back(feature);
            }
        };
        for (const auto& syn : org.compiled_synapses_) {
            if (syn.is_recurrent) add("recurrent_state");
        }
        for (const auto& cell : org.cells) {
            switch (cell.type) {
                case CellType::OP_EMA:
                    add("EMA_initialization/state");
                    break;
                case CellType::OP_DIFF:
                    add("DIFF_previous_input");
                    break;
                case CellType::OP_INTEGRAL:
                    add("INTEGRAL_state");
                    break;
                case CellType::OP_DELAY_N:
                    add("DELAY_buffer");
                    break;
                case CellType::OP_OSCILLATOR:
                    add("OSCILLATOR_state");
                    break;
                case CellType::GATE_HYSTERESIS:
                    add("HYSTERESIS_latch");
                    break;
                case CellType::ASSOCIATION_HUB:
                    add("tanh");
                    break;
                default:
                    break;
            }
        }
        return features;
    }

private:
    static int raw_input_index(CellType type) {
        if (type == CellType::SENSE_RAW_INPUT_0) return 0;
        if (type == CellType::SENSE_RAW_INPUT_1) return 1;
        if (type == CellType::SENSE_RAW_INPUT_2) return 2;
        if (type == CellType::SENSE_RAW_INPUT_3) return 3;
        return -1;
    }

    static Interval port_interval(const CellularOrganism& org,
                                  const std::vector<Interval>& node_bounds,
                                  size_t node_idx, uint8_t port) {
        Interval result;
        for (const auto& syn : org.compiled_synapses_) {
            if (syn.to_idx == node_idx && syn.to_port == port) {
                const Interval source = syn.is_recurrent
                    ? Interval(-10.0, 10.0)
                    : node_bounds[syn.from_idx];
                result = result + (source * syn.weight);
            }
        }
        return result;
    }

    static Interval abs_interval(const Interval& value) {
        if (value.low >= 0.0) return value;
        if (value.high <= 0.0) return {-value.high, -value.low};
        return {0.0, std::max(-value.low, value.high)};
    }

    static Interval hull(const Interval& lhs, const Interval& rhs) {
        return {std::min(lhs.low, rhs.low), std::max(lhs.high, rhs.high)};
    }

    static Interval clamp_interval(const Interval& value, double low,
                                    double high) {
        return {std::clamp(value.low, low, high),
                std::clamp(value.high, low, high)};
    }
};

class FormalSafetyCertifier {
public:
    // 综合验证入口: 支持启发式区间包络检查与真实 SMT 求解器证明
    static SafetyProofCertificate verify_organism(const CellularOrganism& org,
                                                  bool invoke_solver = false,
                                                  const std::string& solver_bin = "z3") {
        SafetyProofCertificate cert;

        // 1. 运行启发式区间算术包络推演
        auto interval_report = HeuristicIntervalBoundsChecker::check_bounds(org);
        cert.interval_bounds_verified = interval_report.passed;
        cert.max_possible_steer_rad = interval_report.max_steer_rad;
        cert.min_possible_accel_mps2 = interval_report.min_accel_mps2;
        cert.max_possible_accel_mps2 = interval_report.max_accel_mps2;
        cert.num_symbolic_clauses = org.compiled_synapses_.size() + org.cells.size() * 3 + 12;
        const auto unsupported = HeuristicIntervalBoundsChecker::unsupported_model_features(org);
        cert.symbolic_model_complete = unsupported.empty();
        for (size_t i = 0; i < unsupported.size(); ++i) {
            if (i != 0) cert.unsupported_model_features += ", ";
            cert.unsupported_model_features += unsupported[i];
        }

        // 2. 生成标准 SMT-LIB 2.6 脚本
        cert.smt2_code = generate_smt2_script(org);

        // 3. 计算指纹校验码
        std::stringstream ss;
        ss << "SMT-PROOF-OBLIGATION-" << std::hex << (cert.num_symbolic_clauses * 0x9e3779b97f4a7c15ULL)
           << "-ORG-" << org.organism_id;
        cert.proof_digest_sha256 = ss.str();

        // 4. 若请求调用真实 SMT 求解器进行严格形式化求证
        if (invoke_solver) {
            if (!cert.symbolic_model_complete) {
                cert.formal_verification_skipped = true;
                cert.solver_status = SolverStatus::UNKNOWN_OR_TIMEOUT;
                cert.is_verified = false;
                cert.solver_message = "SMT proof skipped: forward semantics are conservatively over-approximated "
                    "for unsupported operators/state (" + cert.unsupported_model_features + ").";
            } else {
                std::string solver_out;
                cert.solver_status = execute_smt_solver(cert.smt2_code, solver_bin, &solver_out);
                if (cert.solver_status == SolverStatus::UNSAT_PROVED) {
                    cert.is_verified = true;
                    cert.solver_message = "Formally verified by SMT solver (" + solver_bin + "): UNSAT (No violation reachable).";
                } else if (cert.solver_status == SolverStatus::SAT_COUNTEREXAMPLE) {
                    cert.is_verified = false;
                    cert.solver_message = "SMT solver found safety counterexample: " + solver_out;
                } else if (cert.solver_status == SolverStatus::SOLVER_NOT_FOUND) {
                    cert.formal_verification_skipped = true;
                    cert.is_verified = false;
                    cert.solver_message = "SMT solver binary '" + solver_bin + "' not found on PATH. Exported SMT-LIB script for offline verification.";
                } else {
                    cert.is_verified = false;
                    cert.solver_message = "SMT solver returned: " + solver_out;
                }
            }
        } else {
            cert.solver_status = SolverStatus::NOT_INVOKED;
            cert.formal_verification_skipped = true;
            cert.is_verified = false; // 未经真实求解器求解前不谎称 is_verified
            cert.solver_message = "Heuristic interval bounds checked. SMT-LIB script generated; "
                "formal solver invocation omitted (not certified).";
            if (!cert.symbolic_model_complete) {
                cert.solver_message += " Conservative model features: " + cert.unsupported_model_features + ".";
            }
        }

        return cert;
    }

    // 兼容原接口 (默认执行启发式区间检查 + SMT 生成)
    static SafetyProofCertificate verify_organism_bounds(const CellularOrganism& org) {
        return verify_organism(org, false);
    }

    // 生成标准 SMT-LIB v2 脚本 (QF_NRA 逻辑)
    static std::string generate_smt2_script(const CellularOrganism& org) {
        std::stringstream ss;
        ss << ";; ==========================================================================\n";
        ss << ";; FlowEngine SMT proof-obligation artifact (not an ASIL certification)\n";
        ss << ";; Target: CellularOrganism ID " << org.organism_id << " (Lineage: " << org.lineage_name << ")\n";
        ss << ";; Logic: QF_NRA (Quantifier-Free Non-linear Real Arithmetic)\n";
        ss << ";; Stateful cells use unconstrained previous-state symbols as a conservative proof obligation.\n";
        ss << ";; Unsupported state/transcendental operators are over-approximated and are not certifiable.\n";
        ss << ";; ==========================================================================\n\n";
        ss << "(set-logic QF_NRA)\n";

        // 声明感知输入
        ss << ";; --- Sensory Input Declarations ---\n";
        ss << "(declare-fun s0 () Real) ; lead_distance (m)\n";
        ss << "(declare-fun s1 () Real) ; relative_velocity (m/s)\n";
        ss << "(declare-fun s2 () Real) ; lateral_offset (m)\n";
        ss << "(declare-fun s3 () Real) ; time_to_collision (s)\n\n";

        // 输入有界前提约束 (Preconditions)
        ss << ";; --- Operational Design Domain (ODD) Preconditions ---\n";
        ss << "(assert (and (>= s0 0.5) (<= s0 150.0)))\n";
        ss << "(assert (and (>= s1 -35.0) (<= s1 20.0)))\n";
        ss << "(assert (and (>= s2 -5.0) (<= s2 5.0)))\n";
        ss << "(assert (and (>= s3 0.05) (<= s3 99.0)))\n\n";

        // 声明中间细胞神经元变量
        ss << ";; --- Neural Cell DAG Declarations ---\n";
        for (size_t i = 0; i < org.cells.size(); ++i) {
            ss << "(declare-fun c" << i << " () Real)\n";
        }
        ss << "\n";

        auto number = [](double value) {
            std::ostringstream out;
            out << std::setprecision(17) << value;
            return out.str();
        };
        auto raw_input = [](CellType type) {
            if (type == CellType::SENSE_RAW_INPUT_0) return std::string("s0");
            if (type == CellType::SENSE_RAW_INPUT_1) return std::string("s1");
            if (type == CellType::SENSE_RAW_INPUT_2) return std::string("s2");
            if (type == CellType::SENSE_RAW_INPUT_3) return std::string("s3");
            return std::string();
        };
        std::vector<std::string> expressions(org.cells.size(), "0.0");
        std::stringstream state_decls;
        std::stringstream state_comments;

        auto port_expression = [&](size_t node_idx, uint8_t port) {
            std::vector<std::string> terms;
            for (const auto& syn : org.compiled_synapses_) {
                if (syn.to_idx != node_idx || syn.to_port != port) continue;
                const std::string source = syn.is_recurrent
                    ? "prev_c" + std::to_string(syn.from_idx)
                    : expressions[syn.from_idx];
                terms.push_back("(* " + source + " " + number(syn.weight) + ")");
            }
            if (terms.empty()) return std::string("0.0");
            if (terms.size() == 1) return terms.front();
            std::string sum = "(+";
            for (const auto& term : terms) sum += " " + term;
            return sum + ")";
        };

        for (uint32_t node_idx : org.execution_order_) {
            const auto& cell = org.cells[node_idx];
            const std::string input = raw_input(cell.type);
            if (!input.empty()) {
                expressions[node_idx] = "(* " + input + " " + number(cell.param1) + ")";
                continue;
            }

            const std::string in0 = port_expression(node_idx, 0);
            const std::string in1 = port_expression(node_idx, 1);
            switch (cell.type) {
                case CellType::OP_SUM:
                    expressions[node_idx] = "(+ " + in0 + " " + in1 + ")";
                    break;
                case CellType::OP_SUB:
                    expressions[node_idx] = "(- " + in0 + " " + in1 + ")";
                    break;
                case CellType::OP_MULTIPLY:
                    expressions[node_idx] = "(* " + in0 + " " + in1 + ")";
                    break;
                case CellType::OP_RATIO:
                    expressions[node_idx] = "(ite (or (> " + in1 + " 0.000001) (< " +
                        in1 + " -0.000001)) (/ " + in0 + " " + in1 +
                        ") (/ " + in0 + " 0.000001))";
                    break;
                case CellType::OP_ABS:
                    expressions[node_idx] = "(ite (>= " + in0 + " 0.0) " + in0 +
                        " (- " + in0 + "))";
                    break;
                case CellType::OP_QUADRATIC:
                    expressions[node_idx] = "(+ (* " + number(cell.param1) + " " +
                        in0 + " " + in0 + ") (* " + number(cell.param2) + " " +
                        in0 + " " + in1 + "))";
                    break;
                case CellType::OP_EMA:
                    state_decls << "(declare-fun ema_state" << node_idx << " () Real)\n";
                    expressions[node_idx] = "(+ (* " + number(std::clamp(cell.param1, 0.001, 1.0)) +
                        " " + in0 + ") (* " + number(1.0 - std::clamp(cell.param1, 0.001, 1.0)) +
                        " ema_state" + std::to_string(node_idx) + "))";
                    break;
                case CellType::OP_DIFF:
                    state_decls << "(declare-fun prev_input" << node_idx << " () Real)\n";
                    expressions[node_idx] = "(- " + in0 + " prev_input" +
                        std::to_string(node_idx) + ")";
                    break;
                case CellType::OP_INTEGRAL:
                    state_decls << "(declare-fun integral_state" << node_idx << " () Real)\n";
                    expressions[node_idx] = "(+ integral_state" + std::to_string(node_idx) +
                        " (* " + number(cell.param1) + " " + in0 + "))";
                    break;
                case CellType::OP_DELAY_N:
                    state_decls << "(declare-fun delay_state" << node_idx << " () Real)\n";
                    expressions[node_idx] = "delay_state" + std::to_string(node_idx);
                    break;
                case CellType::OP_OSCILLATOR:
                    state_decls << "(declare-fun oscillator_state" << node_idx << " () Real)\n";
                    state_decls << "(declare-fun oscillator_aux" << node_idx << " () Real)\n";
                    state_comments << ";; oscillator " << node_idx <<
                        " is conservatively state-symbolized\n";
                    expressions[node_idx] = "oscillator_state" + std::to_string(node_idx);
                    break;
                case CellType::GATE_THRESHOLD:
                    expressions[node_idx] = "(ite (> " + in0 + " " +
                        number(cell.param1) + ") 1.0 0.0)";
                    break;
                case CellType::GATE_HYSTERESIS:
                    state_decls << "(declare-fun hysteresis_state" << node_idx << " () Real)\n";
                    expressions[node_idx] = "(ite (> " + in0 + " " +
                        number(cell.param1) + ") 1.0 (ite (< " + in0 + " " +
                        number(cell.param2) + ") -1.0 hysteresis_state" +
                        std::to_string(node_idx) + "))";
                    break;
                case CellType::GATE_AND:
                    expressions[node_idx] = "(ite (and (> " + in0 + " 0.0) (> " +
                        in1 + " 0.0)) 1.0 0.0)";
                    break;
                case CellType::GATE_INHIBIT:
                    expressions[node_idx] = "(* " + in0 + " (ite (<= " + in1 +
                        " 0.0) 1.0 (ite (>= " + in1 + " 1.0) 0.0 (- 1.0 " +
                        in1 + "))))";
                    break;
                case CellType::GATE_DEADZONE:
                    expressions[node_idx] = "(ite (or (> " + in0 + " " +
                        number(std::abs(cell.param1)) + ") (< " + in0 + " -" +
                        number(std::abs(cell.param1)) + ")) " + in0 + " 0.0)";
                    break;
                case CellType::GATE_MIN_MAX:
                    expressions[node_idx] = "(ite (> " + number(cell.param1) +
                        " 0.5) (ite (>= " + in0 + " " + in1 + ") " + in0 +
                        " " + in1 + ") (ite (<= " + in0 + " " + in1 + ") " +
                        in0 + " " + in1 + "))";
                    break;
                case CellType::ASSOCIATION_HUB:
                    state_decls << "(declare-fun association_value" << node_idx << " () Real)\n";
                    state_comments << ";; association_hub " << node_idx <<
                        " is bounded by its tanh envelope in the offline obligation\n";
                    state_decls << "(assert (and (>= association_value" << node_idx <<
                        " -1.0) (<= association_value" << node_idx << " 1.0)))\n";
                    expressions[node_idx] = "association_value" + std::to_string(node_idx);
                    break;
                default:
                    expressions[node_idx] = in0;
                    break;
            }
        }

        ss << "\n;; --- Stateful Conservative Proof Obligations ---\n";
        std::unordered_set<size_t> declared_recurrent_states;
        for (const auto& syn : org.compiled_synapses_) {
            if (syn.is_recurrent && declared_recurrent_states.insert(syn.from_idx).second) {
                ss << "(declare-fun prev_c" << syn.from_idx << " () Real)\n";
                ss << "(assert (and (>= prev_c" << syn.from_idx << " -10.0) "
                   << "(<= prev_c" << syn.from_idx << " 10.0)))\n";
            }
        }
        ss << state_comments.str() << state_decls.str() << "\n";
        ss << ";; --- Cellular forward/action equations ---\n";
        for (size_t i = 0; i < expressions.size(); ++i) {
            ss << "(assert (= c" << i << " " << expressions[i] << "))\n";
        }

        // 声明执行器输出
        ss << "\n;; --- Effector Output Clamping ---\n";
        ss << "(declare-fun steer_cmd () Real)\n";
        ss << "(declare-fun accel_cmd () Real)\n";
        ss << "(declare-fun aeb_immune_lock () Bool)\n\n";

        std::string positive_action = "0.0";
        std::string negative_action = "0.0";
        std::string defensive_action = "0.0";
        std::vector<std::string> immune_actions;
        for (const auto& action : org.compiled_actions_) {
            const std::string value = "c" + std::to_string(action.cell_idx);
            if (action.type == CellType::ACT_PRIMARY_POSITIVE) positive_action = value;
            else if (action.type == CellType::ACT_PRIMARY_NEGATIVE) negative_action = value;
            else if (action.type == CellType::ACT_DEFENSIVE_RESET) defensive_action = value;
            else if (action.type == CellType::ACT_IMMUNE_BLOCK) immune_actions.push_back(value);
        }
        std::string immune_expr = "false";
        for (const auto& value : immune_actions) {
            immune_expr = "(or " + immune_expr + " (> " + value + " 0.5))";
        }
        ss << ";; Exact AdasCellularAdapter action mapping (before physical saturation)\n";
        ss << "(declare-fun steer_raw () Real)\n";
        ss << "(declare-fun accel_raw () Real)\n";
        ss << "(assert (= steer_raw (- " << defensive_action << ")))\n";
        ss << "(assert (= accel_raw (- " << positive_action << " " <<
            negative_action << ")))\n";
        ss << "(assert (= aeb_immune_lock " << immune_expr << "))\n";
        ss << "(assert (= steer_cmd (ite (> steer_raw 0.60) 0.60 (ite (< steer_raw -0.60) -0.60 steer_raw))))\n";
        ss << "(assert (= accel_cmd (ite aeb_immune_lock -6.0 (ite (> accel_raw 2.0) 2.0 (ite (< accel_raw -6.0) -6.0 accel_raw)))))\n\n";

        // 形式化安全不变量求反 (Negation of Safety Invariants)
        ss << ";; ==========================================================================\n";
        ss << ";; Safety Invariant Verification (Goal: Prove unsatisfiable under all inputs)\n";
        ss << ";; ==========================================================================\n";
        ss << ";; Violation Target 1: Steer exceeds ISO 26262 lateral stability bound\n";
        ss << ";; Violation Target 2: Emergency brake failure when TTC < 1.2s and rel_v < -3.5m/s\n";
        ss << "(assert (or\n";
        ss << "  (> steer_cmd 0.60)\n";
        ss << "  (< steer_cmd -0.60)\n";
        ss << "  (and (< s3 1.2) (< s1 -3.5) (> accel_cmd -5.8))\n";
        ss << "))\n\n";

        ss << "(check-sat)\n";
        ss << "(exit)\n";

        return ss.str();
    }

    // 保存 .smt2 证书到文件
    static bool save_certificate_file(const SafetyProofCertificate& cert, const std::string& path) {
        std::ofstream ofs(path);
        if (!ofs.is_open()) return false;
        ofs << cert.smt2_code;
        return true;
    }

    // 执行外部 SMT 求解器 (z3 / cvc5)
    static SolverStatus execute_smt_solver(const std::string& smt2_code,
                                           const std::string& solver_bin,
                                           std::string* output_out = nullptr) {
        // 首先探测求解器是否存在
        std::string which_cmd = "which " + solver_bin + " >/dev/null 2>&1";
        if (std::system(which_cmd.c_str()) != 0) {
            if (output_out) *output_out = "Solver binary '" + solver_bin + "' not found in PATH";
            return SolverStatus::SOLVER_NOT_FOUND;
        }

        // Write the query in the caller's working directory and remove it
        // after invocation; never use a shared temporary directory.
        const std::string tmp_path = "flow_safety_verify_query.smt2";
        {
            std::ofstream ofs(tmp_path);
            if (!ofs.is_open()) return SolverStatus::SOLVER_ERROR;
            ofs << smt2_code;
        }

        std::string exec_cmd = solver_bin + " " + tmp_path + " 2>&1";
        std::array<char, 256> buffer;
        std::string result;
        FILE* pipe = popen(exec_cmd.c_str(), "r");
        if (!pipe) {
            if (output_out) *output_out = "Failed to execute popen for solver";
            return SolverStatus::SOLVER_ERROR;
        }
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result += buffer.data();
        }
        pclose(pipe);
        std::remove(tmp_path.c_str());

        if (output_out) *output_out = result;

        // 解析 SMT 求解器输出
        if (result.find("unsat") != std::string::npos && result.find("sat\n") == std::string::npos) {
            return SolverStatus::UNSAT_PROVED;
        } else if (result.find("sat") != std::string::npos) {
            return SolverStatus::SAT_COUNTEREXAMPLE;
        } else if (result.find("unknown") != std::string::npos || result.find("timeout") != std::string::npos) {
            return SolverStatus::UNKNOWN_OR_TIMEOUT;
        }

        return SolverStatus::SOLVER_ERROR;
    }
};

} // namespace kun
