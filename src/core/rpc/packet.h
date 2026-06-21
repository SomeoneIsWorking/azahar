// Copyright 2018-2025 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <functional>
#include <span>
#include "common/common_types.h"

namespace Core::RPC {

enum class PacketType : u32 {
    Undefined = 0,
    ReadMemory = 1,
    WriteMemory = 2,
    ProcessList = 3,
    SetGetProcess = 4,
    // SoH3D oracle (#89): capture the next rendered frame to a PPM file on the host.
    // arg1 = resolution scale (0 = current), arg2 = path byte length; path bytes follow the 8B args.
    Screenshot = 5,
    // SoH3D oracle (#89): set the HELD 3DS pad state for headless input scripting.
    // arg1 = PadState button bits; arg2 = circle pad: bit24 = active, [15:8] = cx %, [7:0] = cy %
    // (signed -100..100, scaled to circle-pad range). Persists until the next Input packet.
    Input = 6,
    // SoH3D oracle (#89): set the held bottom-screen touch. arg1 = active (0/1),
    // arg2 = (x << 16) | y in pixels (x 0..319, y 0..239). Persists until the next Touch packet.
    Touch = 7,
    // SoH3D oracle (#89): save/load an Azahar savestate. arg1 = slot, arg2 = op (0 = save, 1 = load).
    // Lets RE iterations reload an in-game scene instantly instead of re-driving the intro.
    Savestate = 8,
};

struct PacketHeader {
    u32 version;
    u32 id;
    PacketType packet_type;
    u32 packet_size;
};

#pragma pack(push, 1)
struct ProcessInfo {
    u32 process_id;
    u64 title_id;
    std::array<u8, 8> process_name;
};
static_assert(sizeof(ProcessInfo) == 0x14, "Incorrect ProcessInfo size");
#pragma pack(pop)

constexpr u32 CURRENT_VERSION = 1;
constexpr u32 MIN_PACKET_SIZE = sizeof(PacketHeader);
constexpr u32 MAX_PACKET_DATA_SIZE = 1024;
constexpr u32 MAX_PACKET_SIZE = MIN_PACKET_SIZE + MAX_PACKET_DATA_SIZE;
constexpr u32 MAX_READ_SIZE = MAX_PACKET_DATA_SIZE;
constexpr u32 MAX_PROCESSES_IN_LIST = (MAX_PACKET_DATA_SIZE - sizeof(u32)) / sizeof(ProcessInfo);

class Packet {
public:
    explicit Packet(const PacketHeader& header, u8* data,
                    std::function<void(Packet&)> send_reply_callback);
    ~Packet();

    u32 GetVersion() const {
        return header.version;
    }

    u32 GetId() const {
        return header.id;
    }

    PacketType GetPacketType() const {
        return header.packet_type;
    }

    u32 GetPacketDataSize() const {
        return header.packet_size;
    }

    const PacketHeader& GetHeader() const {
        return header;
    }

    std::span<u8, MAX_PACKET_DATA_SIZE> GetPacketData() {
        return packet_data;
    }

    void SetPacketDataSize(u32 size) {
        header.packet_size = size;
    }

    void SendReply() {
        send_reply_callback(*this);
    }

private:
    struct PacketHeader header;
    std::array<u8, MAX_PACKET_DATA_SIZE> packet_data;

    std::function<void(Packet&)> send_reply_callback;
};

} // namespace Core::RPC
