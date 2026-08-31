#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include "kun/cellular/ecosystem_biosphere.hpp"

using namespace kun;

void test_niche_initialization_and_diversity() {
    std::cout << "[Test 1] 运行生态位物种初始化与香农生物多样性指数测试...\n";
    EcoBiosphere biosphere(10, 9999);

    auto counts = biosphere.get_niche_population();
    std::cout << "  ↳ 初始各生态位种群丰度:\n";
    std::cout << "     🌿 生产者 (做市商): " << counts[SpeciesNiche::PRODUCER] << "\n";
    std::cout << "     🐇 初级消费者 (趋势派): " << counts[SpeciesNiche::HERBIVORE] << "\n";
    std::cout << "     🦅 顶级掠食者 (套利派): " << counts[SpeciesNiche::PREDATOR] << "\n";
    std::cout << "     🍄 分解者 (清算派): " << counts[SpeciesNiche::DECOMPOSER] << "\n";

    assert(counts[SpeciesNiche::PRODUCER] == 10);
    assert(counts[SpeciesNiche::HERBIVORE] == 10);
    assert(counts[SpeciesNiche::PREDATOR] == 10);
    assert(counts[SpeciesNiche::DECOMPOSER] == 10);

    double h = biosphere.calculate_shannon_diversity();
    std::cout << "  ↳ 初始香农生物多样性指数 H = " << h << " (理论均值 1.386)\n";
    assert(h > 1.3);

    std::cout << "  -> 生态位初始化与香农多样性测试 100% 通过!\n";
}

void test_trophic_energy_transfer_and_predation() {
    std::cout << "[Test 2] 运行食物网掠食与共生能量流动测试...\n";
    EcoBiosphere biosphere(15, 8888);

    biosphere.step_ecosystem(1.0);
    const auto& transfers = biosphere.recent_transfers();
    std::cout << "  ↳ 单步生态周期产生 " << transfers.size() << " 笔营养级能量转移事件\n";
    assert(!transfers.empty());

    bool has_grazing = false;
    bool has_predation = false;
    for (const auto& t : transfers) {
        if (t.transfer_type == "LIQUIDITY_GRAZING") has_grazing = true;
        if (t.transfer_type == "PREDATION") has_predation = true;
    }

    std::cout << "  ↳ 初级消费者流动性吸纳 (Liquidity Grazing): " << (has_grazing ? "触发成功" : "未触发") << "\n";
    std::cout << "  ↳ 顶级掠食者单腿捕食 (Predation): " << (has_predation ? "触发成功" : "未触发") << "\n";

    assert(has_grazing);
    assert(has_predation);

    std::cout << "  -> 食物网掠食与共生能量流动测试 100% 通过!\n";
}

void test_biome_climate_transition_and_self_balancing() {
    std::cout << "[Test 3] 运行生境气候季相轮替与物种动态自平衡测试...\n";
    EcoBiosphere biosphere(8, 7777);

    // 运行 600 个生态周期
    for (int i = 0; i < 600; ++i) {
        biosphere.step_ecosystem(0.5);
    }

    auto counts = biosphere.get_niche_population();
    std::cout << "  ↳ 600 个周期后各生态位种群自平衡态:\n";
    std::cout << "     🌿 生产者 (做市商): " << counts[SpeciesNiche::PRODUCER] << "\n";
    std::cout << "     🐇 初级消费者 (趋势派): " << counts[SpeciesNiche::HERBIVORE] << "\n";
    std::cout << "     🦅 顶级掠食者 (套利派): " << counts[SpeciesNiche::PREDATOR] << "\n";
    std::cout << "     🍄 分解者 (清算派): " << counts[SpeciesNiche::DECOMPOSER] << "\n";

    // 验证无任何物种彻底灭绝
    assert(counts[SpeciesNiche::PRODUCER] >= 3);
    assert(counts[SpeciesNiche::HERBIVORE] >= 3);
    assert(counts[SpeciesNiche::PREDATOR] >= 3);
    assert(counts[SpeciesNiche::DECOMPOSER] >= 3);

    std::cout << "  -> 生境气候季相轮替与物种自平衡测试 100% 通过!\n";
}

void test_biosphere_json_serialization() {
    std::cout << "[Test 4] 运行 3D 全息生态圈 JSON 序列化测试...\n";
    EcoBiosphere biosphere(5, 6666);
    biosphere.step_ecosystem(1.0);

    std::string json = biosphere.to_json();
    assert(json.find("\"shannon_diversity\"") != std::string::npos);
    assert(json.find("\"biomes\"") != std::string::npos);
    assert(json.find("\"niche_counts\"") != std::string::npos);
    assert(json.find("\"agents\"") != std::string::npos);

    std::cout << "  ↳ 全息 JSON 导出有效 (字节数: " << json.size() << " bytes)\n";
    std::cout << "  -> 3D 全息生态圈 JSON 序列化测试 100% 通过!\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << "     宏观自适应生态圈引擎 (EcoBiosphere) 单元测试集       \n";
    std::cout << "=========================================================\n\n";

    test_niche_initialization_and_diversity();
    test_trophic_energy_transfer_and_predation();
    test_biome_climate_transition_and_self_balancing();
    test_biosphere_json_serialization();

    std::cout << "\n=========================================================\n";
    std::cout << "       生态圈全部 4 组单元测试 100% 满分通过!            \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
