// peggle2-nx (tools/renut_hook_remap.py): the addresses both Renut's renderer and Peggle 2's own code hook.
// Peggle 2's hook calls renut_native_sub_X first when Renut's renderer is chosen (BANJO_RENUT_YIELD_SHARED).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

extern "C" void renut_native_sub_82613298(PPCContext& ctx, uint8_t* base);
extern "C" void renut_native_sub_82613480(PPCContext& ctx, uint8_t* base);
extern "C" void renut_native_sub_82617E58(PPCContext& ctx, uint8_t* base);
extern "C" void renut_native_sub_8261D378(PPCContext& ctx, uint8_t* base);
extern "C" void renut_native_sub_8261EE00(PPCContext& ctx, uint8_t* base);
extern "C" void renut_native_sub_8261FE50(PPCContext& ctx, uint8_t* base);
extern "C" void renut_native_sub_826221A0(PPCContext& ctx, uint8_t* base);
extern "C" void renut_native_sub_82622660(PPCContext& ctx, uint8_t* base);
extern "C" void renut_native_sub_826226A8(PPCContext& ctx, uint8_t* base);
extern "C" void renut_native_sub_826233C8(PPCContext& ctx, uint8_t* base);
