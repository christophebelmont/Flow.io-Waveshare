#pragma once

#include "Modules/PoolDeviceModule/PoolDeviceModuleDataModel.h"

namespace PoolDeviceSlots {

inline constexpr PoolDeviceSlotDescriptor kSlots[] = {
    {"pd0", "pdm/pd0", "act0_en", "act0_dp", "act0_flh", "act0_tc", "act0_ti", "act0_mu", "act0_rt"},
    {"pd1", "pdm/pd1", "act1_en", "act1_dp", "act1_flh", "act1_tc", "act1_ti", "act1_mu", "act1_rt"},
    {"pd2", "pdm/pd2", "act2_en", "act2_dp", "act2_flh", "act2_tc", "act2_ti", "act2_mu", "act2_rt"},
    {"pd3", "pdm/pd3", "act3_en", "act3_dp", "act3_flh", "act3_tc", "act3_ti", "act3_mu", "act3_rt"},
    {"pd4", "pdm/pd4", "act4_en", "act4_dp", "act4_flh", "act4_tc", "act4_ti", "act4_mu", "act4_rt"},
    {"pd5", "pdm/pd5", "act5_en", "act5_dp", "act5_flh", "act5_tc", "act5_ti", "act5_mu", "act5_rt"},
    {"pd6", "pdm/pd6", "act6_en", "act6_dp", "act6_flh", "act6_tc", "act6_ti", "act6_mu", "act6_rt"},
    {"pd7", "pdm/pd7", "act7_en", "act7_dp", "act7_flh", "act7_tc", "act7_ti", "act7_mu", "act7_rt"},
    {"pd8", "pdm/pd8", "act8_en", "act8_dp", "act8_flh", "act8_tc", "act8_ti", "act8_mu", "act8_rt"},
    {"pd9", "pdm/pd9", "act9_en", "act9_dp", "act9_flh", "act9_tc", "act9_ti", "act9_mu", "act9_rt"},
    {"pd10", "pdm/pd10", "act10_en", "act10_dp", "act10_flh", "act10_tc", "act10_ti", "act10_mu", "act10_rt"},
    {"pd11", "pdm/pd11", "act11_en", "act11_dp", "act11_flh", "act11_tc", "act11_ti", "act11_mu", "act11_rt"},
    {"pd12", "pdm/pd12", "act12_en", "act12_dp", "act12_flh", "act12_tc", "act12_ti", "act12_mu", "act12_rt"},
    {"pd13", "pdm/pd13", "act13_en", "act13_dp", "act13_flh", "act13_tc", "act13_ti", "act13_mu", "act13_rt"},
    {"pd14", "pdm/pd14", "act14_en", "act14_dp", "act14_flh", "act14_tc", "act14_ti", "act14_mu", "act14_rt"},
    {"pd15", "pdm/pd15", "act15_en", "act15_dp", "act15_flh", "act15_tc", "act15_ti", "act15_mu", "act15_rt"},
};

static_assert((sizeof(kSlots) / sizeof(kSlots[0])) >= POOL_DEVICE_MAX,
              "PoolDevice fixed slot descriptors must cover POOL_DEVICE_MAX");

}  // namespace PoolDeviceSlots
