#pragma once

#include <array>
#include <format>
#include <xtensor/containers/xtensor.hpp>

struct PacketHeader
{
    static constexpr  uint32_t MAGIC = 0xA1B2C3D4;
    uint32_t magic;
    uint32_t counter;
    uint32_t packet_size_bytes;
    uint32_t ignored;
};
static_assert(sizeof(PacketHeader) == 16);

struct PacketFooter : public std::array<uint64_t, 2> {
    static constexpr uint64_t MAGIC[] = {0xDEADBEEFCAFEBABE, 0x0123456789ABCDEF};
};
static_assert(sizeof(PacketHeader) == 16);

class Packet
{
public:
    Packet() = default;
    Packet(Packet &&) = default;
    Packet& operator=(Packet&&) = default;
    ~Packet() = default;
    PacketHeader header;
    xt::xtensor<int16_t,2> data;
    PacketFooter footer;
};

template <>
struct std::formatter<PacketHeader> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(PacketHeader const &header, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "PacketHeader({:#8X}, {}, {}, {:#8X})",
            header.magic, header.counter, header.packet_size_bytes, header.ignored);
    }
};

template <>
struct std::formatter<PacketFooter> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(PacketFooter const &footer, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "PacketFooter({:#16X}, {:#16X})",
            footer[0], footer[1]);
    }
};
