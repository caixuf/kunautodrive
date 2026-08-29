#include "kun/core/config_loader.hpp"
#include <fstream>
#include <sstream>
#include <iostream>

namespace kun {

QuantAppConfig ConfigLoader::default_config() {
    QuantAppConfig cfg;
    cfg.server.port = 8900;
    cfg.server.static_dir = "tools/kunboard";
    cfg.server.db_path = "data/kun_quant.db";

    cfg.accounts.push_back({"acc_master_simnow", "SimNow期货仿真", AccountRole::MASTER, 1.0, 1000000.0});
    cfg.accounts.push_back({"acc_slave_zhongxin", "中信期货实盘", AccountRole::SLAVE, 1.5, 1500000.0});
    cfg.accounts.push_back({"acc_slave_yongan", "永安期货实盘", AccountRole::SLAVE, 0.8, 800000.0});

    cfg.symbols.push_back({"rb2405", "SHFE", 10, 1.0, 0.10, 0.0001});
    cfg.symbols.push_back({"cu2405", "SHFE", 5, 10.0, 0.12, 0.00005});
    cfg.symbols.push_back({"ag2405", "SHFE", 15, 1.0, 0.12, 0.00005});

    return cfg;
}

bool ConfigLoader::load_from_file(const std::string& path, QuantAppConfig& out_cfg) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[ConfigLoader] Cannot open config file: " << path << ", falling back to defaults.\n";
        out_cfg = default_config();
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // 简单高效解析 JSON 核心配置 (或直接使用默认配置并按字段覆盖)
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

    return true;
}

} // namespace kun
