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
constexpr std::uint32_t Threads = 45;
constexpr std::uint32_t Capacity = 64;
constexpr std::uint32_t Sentinel = 0xdeadbeefu;
alignas(256) std::array<std::uint32_t, Threads> Input{};
alignas(256) std::array<std::uint32_t, 4 * Capacity> Output{};

alignas(256) constexpr std::array<std::uint32_t, 16> Code{
    0xe0302000u, 0x80000200u,
    0xbf8c3f70u,
    0x7e100502u,
    0x7e060208u,
    0xbeea0308u,
    0x7e0e0281u,
    0x02080e80u,
    0x340c0082u,
    0xe0702000u, 0x80010206u,
    0xe0702004u, 0x80010306u,
    0xe0702008u, 0x80010406u,
    0xbf810000u,
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



        for (std::uint32_t tid = 0; tid < Threads; ++tid) Input[tid] = 0x4e000000u + tid;
        Input[0] = 0x7b36c4a9u;
        Input[8] = 0x03213b3bu;
        Input[16] = 0x48c970c9u;
        Input[24] = 0x84992d84u;
        Input[32] = 0xd8415e93u;
        Input[40] = 0xf2c9a1a1u;
        Output.fill(Sentinel);
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
        for (std::uint32_t first = 0; first < Threads; first += WaveSize) {
            const auto lanes = std::min(WaveSize, Threads - first);
            const auto expected = Input[first];
            const auto activeMask = 0xffffffffu >> (WaveSize - lanes);
            std::uint32_t selected = 0, inputMismatch = 0, scalarMismatch = 0, bitMismatch = 0, stray = 0;
            for (std::uint32_t lane = 0; lane < lanes; ++lane) {
                const auto tid = first + lane;
                const auto at = 4 * tid;
                if (Output[at] != Input[tid]) ++inputMismatch;
                if (Output[at + 1] != expected) ++scalarMismatch;
                if (Output[at + 2] != ((expected >> lane) & 1u)) ++bitMismatch;
                if (Output[at + 2] == 1u) selected |= 1u << lane;
                if (Output[at + 3] != Sentinel) ++stray;
                if (lane % 8 == 0) {
                    std::printf("wave=%u lane=%u loaded=0x%08x readfirstlane=0x%08x\n",
                                first / WaveSize, lane, Output[at], Output[at + 1]);
                }
            }
            std::printf("wave=%u active=%u expected_scalar=0x%08x expected_bits=0x%08x selected=0x%08x "
                        "input_mismatch=%u scalar_mismatch=%u bit_mismatch=%u stray_within_active=%u\n",
                        first / WaveSize, lanes, expected, expected & activeMask, selected,
                        inputMismatch, scalarMismatch, bitMismatch, stray);
            if (inputMismatch || scalarMismatch || bitMismatch || stray) passed = false;
        }
        const auto inactiveWrites = std::count_if(Output.begin() + 4 * Threads, Output.end(), [](auto v) { return v != Sentinel; });
        std::printf("writes_outside_workgroup=%zu\n", static_cast<std::size_t>(inactiveWrites));
        if (inactiveWrites) passed = false;
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
