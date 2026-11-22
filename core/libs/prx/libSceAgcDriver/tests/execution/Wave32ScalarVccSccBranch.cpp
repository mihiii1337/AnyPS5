#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <span>

namespace {

constexpr std::uint32_t WaveSize = 32;
constexpr std::uint32_t Threads = WaveSize;
alignas(256) std::array<std::uint32_t, 4 * Threads> Output{};

alignas(256) constexpr std::array<std::uint32_t, 25> Code{
    0x7d840080u,
    0xbe88036au,
    0xbf078008u,
    0xbf840003u,
    0xbeea03c1u,
    0xbe8a0391u,
    0xbf820003u,
    0xbeea03ffu, 0x000000ffu,
    0xbe8a03aau,
    0x7e0e0281u,
    0x02020e80u,
    0x7e04026au,
    0x7e060208u,
    0x7e08020au,
    0x340c0082u,
    0xe0702000u, 0x80000106u,
    0xe0702004u, 0x80000206u,
    0xe0702008u, 0x80000306u,
    0xe070200cu, 0x80000406u,
    0xbf810000u,
};

std::array<std::uint32_t, 4> BufferDescriptor(std::span<std::uint32_t> values) {
    const auto address = reinterpret_cast<std::uintptr_t>(values.data());
    return {static_cast<std::uint32_t>(address),
            static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u),
            static_cast<std::uint32_t>(values.size()), 0x11016facu};
}

bool Run(AgcDriver::VulkanDevice& device) {
    Output.fill(0xdeadbeefu);
    const auto userData = BufferDescriptor(Output);
    const std::span<const std::uint32_t> code(Code);
    const auto codeAddress = reinterpret_cast<std::uintptr_t>(code.data());
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{codeAddress, std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, codeAddress, code, 0, {}},
        {WaveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128},
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, codeAddress);
    device.WaitIdle();

    std::uint32_t selected = 0, nonzeroArm = 0, zeroArm = 0, ballotOne = 0;
    std::uint32_t bitMismatch = 0, badMarker = 0, armWordMismatch = 0;
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        const auto offset = 4 * lane;
        const auto bit = Output[offset];
        const auto raw = Output[offset + 1];
        const auto ballot = Output[offset + 2];
        const auto marker = Output[offset + 3];
        if (bit == 1u) selected |= 1u << lane;
        else ++bitMismatch;
        if (ballot == 1u) ballotOne |= 1u << lane;
        if (marker == 17u) {
            nonzeroArm |= 1u << lane;
            if (raw != 0xffffffffu) ++armWordMismatch;
        } else if (marker == 42u) {
            zeroArm |= 1u << lane;
            if (raw != 0xffu) ++armWordMismatch;
        } else {
            ++badMarker;
        }
        if (lane % 8u == 0u) {
            std::printf("lane=%u ballot=0x%08x arm=%u raw_vcc=0x%08x\n", lane, ballot, marker, raw);
        }
    }
    std::printf("selected=0x%08x expected=0xffffffff bit_mismatch=%u "
                "nonzero_arm_lanes=0x%08x zero_arm_lanes=0x%08x ballot_one_lanes=0x%08x "
                "bad_marker=%u arm_word_mismatch=%u\n",
                selected, bitMismatch, nonzeroArm, zeroArm, ballotOne, badMarker, armWordMismatch);
    return bitMismatch == 0u && badMarker == 0u && armWordMismatch == 0u;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        return Run(*device) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
