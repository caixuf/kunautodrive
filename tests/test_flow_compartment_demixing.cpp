#include <cassert>
#include <iostream>
#include <cmath>
#include "kun/cellular/cellular_genome.hpp"
#include "kun/cellular/adas_cellular_adapter.hpp"

using namespace kun;

static void test_lj_pulls_synapses_together() {
    std::cout << "[Test 1] 突触弹簧 + 兰纳-琼斯松弛后，耦合对应比随机对更近...\n";
    auto org = CellularOrganism::create_adas_seed_organism(7);
    double r0 = org.compute_mechanical_demixing_ratio();
    for (int k = 0; k < 80; ++k) {
        org.step_force_field_physics(0.02f);
    }
    double r1 = org.compute_mechanical_demixing_ratio();
    std::cout << "  ↳ 分相比 松弛前=" << r0 << " 松弛后=" << r1 << "\n";
    assert(r1 > 1.05);
    assert(r1 >= r0 * 0.95);
    std::cout << "  -> 力学微柱收缩测试通过\n";
}

static void test_adapter_has_no_ttc_bypass() {
    std::cout << "[Test 2] 无免疫突触时，TTC/相对速度不得由适配器旁路刹停...\n";
    auto embryo = CellularOrganism::create_disconnected_embryo(3);
    AdasCellularAdapter adas(std::move(embryo));
    auto ctl = adas.process_perception(6.0, -8.0, 0.4, 0.5);
    assert(!ctl.is_aeb_triggered);
    std::cout << "  -> 适配器无 TTC 旁路\n";
}

static void test_adas_seed_immune_is_in_graph() {
    std::cout << "[Test 3] 智驾原基在 TTC<2 或相对速度过冲时由细胞触发免疫锁...\n";
    auto org = CellularOrganism::create_adas_seed_organism(9);
    AdasCellularAdapter adas(org);
    auto aeb = adas.process_perception(20.0, -6.0, 0.0, 1.2);
    auto cruise = adas.process_perception(80.0, 0.0, 0.0, 20.0);
    assert(aeb.is_aeb_triggered);
    assert(!cruise.is_aeb_triggered);
    std::cout << "  -> 免疫锁在图内，不在适配器\n";
}

int main() {
    test_lj_pulls_synapses_together();
    test_adapter_has_no_ttc_bypass();
    test_adas_seed_immune_is_in_graph();
    std::cout << "compartment demixing: all passed\n";
    return 0;
}
