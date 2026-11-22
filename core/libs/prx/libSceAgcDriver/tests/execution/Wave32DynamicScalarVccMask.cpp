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
alignas(256) std::array<std::uint32_t, 2> Masks{0x7b36c4a9u, 0xd8415e93u};
alignas(256) std::array<std::uint32_t, 2 * Threads> Output{};

alignas(256) constexpr std::array<std::uint32_t, 15> Code{
    0x2c040085u,              // v_lshrrev_b32_e32 v2, 5, v0
    0xe0302000u, 0x80000302u, // buffer_load_dword v3, v2, s[0:3], 0 idxen
    0xbf8c3f70u,              // s_waitcnt vmcnt(0)
    0x7e100503u,              // v_readfirstlane_b32 s8, v3
    0xbeea0308u,              // s_mov_b32 vcc_lo, s8
    0x7e0e0281u,              // v_mov_b32_e32 v7, 1
    0x02020e80u,              // v_cndmask_b32_e32 v1, 0, v7, vcc_lo
    0x7e080208u,              // v_mov_b32_e32 v4, s8
    0x340c0081u,              // v_lshlrev_b32_e32 v6, 1, v0
    0xe0702000u, 0x80010106u, // buffer_store_dword v1, v6, s[4:7], 0 idxen
    0xe0702004u, 0x80010406u, // buffer_store_dword v4, v6, s[4:7], 0 idxen offset:4
    0xbf810000u,              // s_endpgm
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

        Output.fill(0xdeadbeefu);
        std::array<std::uint32_t, 8> userData{};
        const auto input = BufferDescriptor(Masks);
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
        for (std::uint32_t wave = 0; wave < Masks.size(); ++wave) {
            std::uint32_t observed = 0, selectionMismatches = 0, scalarMismatches = 0;
            for (std::uint32_t lane = 0; lane < WaveSize; ++lane) {
                const auto index = 2 * (wave * WaveSize + lane);
                const auto bit = Output[index];
                if (bit == 1) observed |= 1u << lane;
                if (bit != ((Masks[wave] >> lane) & 1u)) ++selectionMismatches;
                if (Output[index + 1] != Masks[wave]) ++scalarMismatches;
            }
            std::printf("wave=%u expected=0x%08x selected=0x%08x selection_mismatches=%u scalar_mismatches=%u\n",
                        wave, Masks[wave], observed, selectionMismatches, scalarMismatches);
            if (selectionMismatches || scalarMismatches) passed = false;
        }
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
