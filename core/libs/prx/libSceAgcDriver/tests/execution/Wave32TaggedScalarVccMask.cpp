#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <span>

namespace {

constexpr std::uint32_t WaveSize = 32;
constexpr std::uint32_t Threads = 2 * WaveSize;
constexpr std::array<std::uint32_t, 2> CompareMasks{0x7b36c4a9u, 0xd8415e93u};
constexpr std::array<std::uint32_t, 2> RawMasks{0xc35a6d92u, 0x2e943bc5u};
constexpr std::array<const char*, 4> Paths{"compare", "vcc_s8_vcc", "vcc_s8_s10_vcc", "raw_s12_vcc"};
alignas(256) std::array<std::uint32_t, 2 * Threads> Input{};
alignas(256) std::array<std::uint32_t, Paths.size() * Threads> Output{};

alignas(256) constexpr std::array<std::uint32_t, 30> Code{
    0x340c0081u, // v_lshlrev_b32_e32 v6, 1, v0
    0xe0302000u, 0x80000306u, // buffer_load_dword v3, v6, s[0:3], 0 idxen
    0xe0302004u, 0x80000406u, // buffer_load_dword v4, v6, s[0:3], 0 idxen offset:4
    0xbf8c3f70u, // s_waitcnt vmcnt(0)
    0x7e180504u, // v_readfirstlane_b32 s12, v4
    0x7e0e0281u, // v_mov_b32_e32 v7, 1
    0x7d8a0680u, // v_cmp_ne_u32_e32 vcc_lo, 0, v3
    0x02100e80u, // v_cndmask_b32_e32 v8, 0, v7, vcc_lo
    0xbe88036au, // s_mov_b32 s8, vcc_lo
    0xbe8a0308u, // s_mov_b32 s10, s8
    0xbeea0380u, // s_mov_b32 vcc_lo, 0
    0xbeea0308u, // s_mov_b32 vcc_lo, s8
    0x02120e80u, // v_cndmask_b32_e32 v9, 0, v7, vcc_lo
    0xbeea0380u, // s_mov_b32 vcc_lo, 0
    0xbeea030au, // s_mov_b32 vcc_lo, s10
    0x02140e80u, // v_cndmask_b32_e32 v10, 0, v7, vcc_lo
    0xbeea030cu, // s_mov_b32 vcc_lo, s12
    0x02160e80u, // v_cndmask_b32_e32 v11, 0, v7, vcc_lo
    0x340c0082u, // v_lshlrev_b32_e32 v6, 2, v0
    0xe0702000u, 0x80010806u, // buffer_store_dword v8, v6, s[4:7], 0 idxen
    0xe0702004u, 0x80010906u, // buffer_store_dword v9, v6, s[4:7], 0 idxen offset:4
    0xe0702008u, 0x80010a06u, // buffer_store_dword v10, v6, s[4:7], 0 idxen offset:8
    0xe070200cu, 0x80010b06u, // buffer_store_dword v11, v6, s[4:7], 0 idxen offset:12
    0xbf810000u, // s_endpgm
};

std::array<std::uint32_t, 4> BufferDescriptor(std::span<std::uint32_t> values) {
    const auto address = reinterpret_cast<std::uintptr_t>(values.data());
    return {static_cast<std::uint32_t>(address),
            static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u),
            static_cast<std::uint32_t>(values.size()), 0x11016facu};
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;

        // The comparison consumes a per-invocation value. Only the separate raw control
        // consumes a wave-uniform packed mask from the input buffer.
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            const auto wave = tid / WaveSize;
            Input[2 * tid] = (CompareMasks[wave] >> (tid % WaveSize)) & 1u;
            Input[2 * tid + 1] = RawMasks[wave];
        }
        Output.fill(0xdeadbeefu);
        std::array<std::uint32_t, 8> userData{};
        const auto input = BufferDescriptor(Input);
        const auto output = BufferDescriptor(Output);
        std::copy(input.begin(), input.end(), userData.begin());
        std::copy(output.begin(), output.end(), userData.begin() + 4);
        const std::span<const std::uint32_t> code(Code);
        const auto codeAddress = reinterpret_cast<std::uintptr_t>(code.data());
        const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{codeAddress, std::as_bytes(code)}}};
        const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
        ShaderRecompiler::RecompileRequest request{
            {ShaderRecompiler::ShaderStage::Compute, codeAddress, code, 0, {}},
            {WaveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
            device->Target(),
            {0, 0, 0, 128},
        };
        request.useCache = false;
        const auto result = ShaderRecompiler::Recompile(request);
        device->Dispatch(result, 1, 1, 1, {}, codeAddress);
        device->WaitIdle();

        bool passed = true;
        for (std::uint32_t wave = 0; wave < CompareMasks.size(); ++wave) {
            for (std::size_t path = 0; path < Paths.size(); ++path) {
                const auto expected = (path == 3 ? RawMasks : CompareMasks)[wave];
                std::uint32_t observed = 0, mismatches = 0;
                for (std::uint32_t lane = 0; lane < WaveSize; ++lane) {
                    const auto bit = Output[Paths.size() * (wave * WaveSize + lane) + path];
                    if (bit == 1u) observed |= 1u << lane;
                    if (bit != ((expected >> lane) & 1u)) ++mismatches;
                }
                std::printf("wave=%u path=%s expected=0x%08x selected=0x%08x mismatches=%u\n",
                            wave, Paths[path], expected, observed, mismatches);
                if (mismatches) passed = false;
            }
        }
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
