#include "kun/core/config_loader.hpp"
#include <cstring>
#include <fstream>
#include <sstream>
#include <iostream>

namespace kun {

QuantAppConfig ConfigLoader::default_config() {
    QuantAppConfig cfg;
    cfg.server.port = 8900;
    cfg.server.static_dir = "tools/kunboard";
    cfg.server.db_path = "data/kun_quant.db";

    // 默认配置一律为纯仿真沙盒账户，严禁内置实盘券商名称与 live=true 标记
    cfg.accounts.push_back({"acc_master_simnow", "SimNow期货仿真", AccountRole::MASTER, 1.0, 1000000.0, false});
    cfg.accounts.push_back({"acc_slave_sim1", "SimNow期货跟单1", AccountRole::SLAVE, 1.5, 1500000.0, false});
    cfg.accounts.push_back({"acc_slave_sim2", "SimNow期货跟单2", AccountRole::SLAVE, 0.8, 800000.0, false});

    cfg.symbols.push_back({"rb2405", "SHFE", 10, 1.0, 0.10, 0.0001, "RB0"});
    cfg.symbols.push_back({"cu2405", "SHFE", 5, 10.0, 0.12, 0.00005, "CU0"});
    cfg.symbols.push_back({"ag2405", "SHFE", 15, 1.0, 0.12, 0.00005, "AG0"});

    return cfg;
}

bool ConfigLoader::load_from_file(const std::string& path, QuantAppConfig& out_cfg) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[ConfigLoader::SECURITY_ERROR] 无法打开配置文件: " << path 
                  << "，拒绝静默回退以防越权！\n";
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    out_cfg = default_config();

    // 提取 port
    size_t port_pos = content.find("\"port\":");
    if (port_pos != std::string::npos) {
        try {
            size_t val_start = content.find_first_of("0123456789", port_pos);
            size_t val_end = content.find_first_of(",}\n\r ", val_start);
            out_cfg.server.port = std::stoi(content.substr(val_start, val_end - val_start));
        } catch (...) {}
    }

    // 提取 ai_port (内部 ai_service 端口)
    size_t ai_port_pos = content.find("\"ai_port\":");
    if (ai_port_pos != std::string::npos) {
        try {
            size_t val_start = content.find_first_of("0123456789", ai_port_pos);
            size_t val_end = content.find_first_of(",}\n\r ", val_start);
            out_cfg.server.ai_port = std::stoi(content.substr(val_start, val_end - val_start));
        } catch (...) {}
    }

    // 提取 static_dir
    size_t dir_pos = content.find("\"static_dir\":");
    if (dir_pos != std::string::npos) {
        size_t first_q = content.find('\"', dir_pos + 13);
        size_t second_q = content.find('\"', first_q + 1);
        if (first_q != std::string::npos && second_q != std::string::npos) {
            out_cfg.server.static_dir = content.substr(first_q + 1, second_q - first_q - 1);
        }
    }

    // 提取 db_path
    size_t db_pos = content.find("\"db_path\":");
    if (db_pos != std::string::npos) {
        size_t first_q = content.find('\"', db_pos + 10);
        size_t second_q = content.find('\"', first_q + 1);
        if (first_q != std::string::npos && second_q != std::string::npos) {
            out_cfg.server.db_path = content.substr(first_q + 1, second_q - first_q - 1);
        }
    }

    // 提取 symbols 数组 (合约专项跟踪/落盘的合约清单来源)
    size_t sym_pos = content.find("\"symbols\":");
    if (sym_pos != std::string::npos) {
        size_t arr_end = content.find(']', sym_pos);
        if (arr_end != std::string::npos) {
            std::string arr = content.substr(sym_pos, arr_end - sym_pos);
            out_cfg.symbols.clear();
            size_t pos = 0;
            while (true) {
                size_t s_pos = arr.find("\"symbol\":", pos);
                if (s_pos == std::string::npos) break;
                size_t obj_end = arr.find('}', s_pos);
                std::string obj = arr.substr(s_pos,
                    obj_end == std::string::npos ? std::string::npos : obj_end - s_pos);

                auto get_str = [&obj](const char* key) -> std::string {
                    size_t k = obj.find(std::string("\"") + key + "\":");
                    if (k == std::string::npos) return "";
                    // 跳过 "key": 共 len(key)+3 个字符, 定位到值的起始引号
                    size_t q1 = obj.find('\"', k + strlen(key) + 3);
                    size_t q2 = (q1 == std::string::npos) ? std::string::npos : obj.find('\"', q1 + 1);
                    return (q1 == std::string::npos || q2 == std::string::npos)
                        ? "" : obj.substr(q1 + 1, q2 - q1 - 1);
                };
                auto get_num = [&obj](const char* key, double def) -> double {
                    size_t k = obj.find(std::string("\"") + key + "\":");
                    if (k == std::string::npos) return def;
                    size_t v1 = obj.find_first_of("0123456789.-", k);
                    if (v1 == std::string::npos) return def;
                    size_t v2 = obj.find_first_of(",}\n\r ", v1);
                    try { return std::stod(obj.substr(v1, v2 - v1)); }
                    catch (...) { return def; }
                };

                SymbolInfo info;
                info.symbol = get_str("symbol");
                info.exchange = get_str("exchange");
                info.multiplier = static_cast<int>(get_num("multiplier", 10.0));
                info.price_tick = get_num("price_tick", 1.0);
                info.margin_ratio = get_num("margin_ratio", 0.10);
                info.commission_ratio = get_num("commission_ratio", 0.0001);
                info.sina_code = get_str("sina_code");
                if (!info.symbol.empty()) out_cfg.symbols.push_back(info);

                pos = s_pos + 9;
            }
        }
    }

    return true;
}

} // namespace kun
