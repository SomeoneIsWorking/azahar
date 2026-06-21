// Copyright 2019-2025 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <fstream>
#include <future>
#include <string>
#include "common/logging/log.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "core/rpc/packet.h"
#include "core/rpc/rpc_server.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace Core::RPC {

RPCServer::RPCServer(Core::System& system_) : system{system_} {
    LOG_INFO(RPC_Server, "Starting RPC server.");
    request_handler_thread =
        std::jthread([this](std::stop_token stop_token) { HandleRequestsLoop(stop_token); });
}

RPCServer::~RPCServer() = default;

void RPCServer::HandleReadMemory(Packet& packet, u32 address, u32 data_size) {
    if (data_size > MAX_READ_SIZE) {
        return;
    }
    u32 read_size = data_size;

    // Note: Memory read occurs asynchronously from the state of the emulator
    if (selected_pid == 0xFFFFFFFF) {
        LOG_ERROR(RPC_Server, "No target process selected, memory access may be invalid.");
        system.Memory().ReadBlock(address, packet.GetPacketData().data(), data_size);
    } else {
        auto process = system.Kernel().GetProcessById(selected_pid);
        if (process) {
            system.Memory().ReadBlock(*process, address, packet.GetPacketData().data(), data_size);
        } else {
            LOG_ERROR(RPC_Server, "Selected process does not exist.");
            read_size = 0;
        }
    }

    packet.SetPacketDataSize(read_size);
    packet.SendReply();
}

void RPCServer::HandleWriteMemory(Packet& packet, u32 address, std::span<const u8> data) {
    // Only allow writing to certain memory regions
    if ((address >= Memory::PROCESS_IMAGE_VADDR && address <= Memory::PROCESS_IMAGE_VADDR_END) ||
        (address >= Memory::HEAP_VADDR && address <= Memory::HEAP_VADDR_END) ||
        (address >= Memory::LINEAR_HEAP_VADDR && address <= Memory::LINEAR_HEAP_VADDR_END) ||
        (address >= Memory::N3DS_EXTRA_RAM_VADDR && address <= Memory::N3DS_EXTRA_RAM_VADDR_END)) {
        // Note: Memory write occurs asynchronously from the state of the emulator
        if (selected_pid == 0xFFFFFFFF) {
            LOG_ERROR(RPC_Server, "No target process selected, memory access may be invalid.");
            system.Memory().WriteBlock(address, data.data(), data.size());
        } else {
            auto process = system.Kernel().GetProcessById(selected_pid);
            if (process) {
                system.Memory().WriteBlock(*process, address, data.data(), data.size());
            } else {
                LOG_ERROR(RPC_Server, "Selected process does not exist.");
            }
        }

        // If the memory happens to be executable code, make sure the changes become visible

        // Is current core correct here?
        system.InvalidateCacheRange(address, data.size());
    }
    packet.SetPacketDataSize(0);
    packet.SendReply();
}

void RPCServer::HandleProcessList(Packet& packet, u32 start_index, u32 max_amount) {
    const auto process_list = system.Kernel().GetProcessList();
    const u32 start = std::min(start_index, static_cast<u32>(process_list.size()));
    const u32 end = std::min(start + max_amount, static_cast<u32>(process_list.size()));
    const u32 count = std::min(end - start, MAX_PROCESSES_IN_LIST);

    u8* out_data = packet.GetPacketData().data();
    u32 written_bytes = 0;

    memcpy(out_data + written_bytes, &count, sizeof(count));
    written_bytes += sizeof(count);

    for (u32 i = start; i < start + count; i++) {
        ProcessInfo info{};
        info.process_id = process_list[i]->process_id;
        info.title_id = process_list[i]->codeset->program_id;
        memcpy(info.process_name.data(), process_list[i]->codeset->name.data(),
               std::min(process_list[i]->codeset->name.size(), info.process_name.size()));

        memcpy(out_data + written_bytes, &info, sizeof(ProcessInfo));
        written_bytes += sizeof(ProcessInfo);
    }

    packet.SetPacketDataSize(written_bytes);
    packet.SendReply();
}

void RPCServer::HandleSetGetProcess(Packet& packet, u32 operation, u32 process_id) {
    u8* out_data = packet.GetPacketData().data();
    u32 written_bytes = 0;

    if (operation == 0) {
        // Get
        memcpy(out_data + written_bytes, &selected_pid, sizeof(selected_pid));
        written_bytes += sizeof(selected_pid);
    } else {
        // Set
        selected_pid = process_id;
    }

    packet.SetPacketDataSize(written_bytes);
    packet.SendReply();
}

// SoH3D oracle (#89): capture the next rendered frame to a PPM file. Runs on the RPC thread; the
// renderer fills our buffer on its next frame (emulation keeps running, independent of this thread),
// so we block on a promise set by the completion callback, then write the PPM ourselves (no Qt).
void RPCServer::HandleScreenshot(Packet& packet, u32 res_scale, std::span<const u8> path) {
    std::string out_path(reinterpret_cast<const char*>(path.data()), path.size());
    auto& renderer = system.GPU().Renderer();
    if (res_scale == 0) {
        res_scale = renderer.GetResolutionScaleFactor();
    }
    const auto layout = Layout::FrameLayoutFromResolutionScale(res_scale, false);
    std::vector<u8> pixels(static_cast<size_t>(layout.width) * layout.height * 4);

    std::promise<bool> done;
    auto fut = done.get_future();
    renderer.RequestScreenshot(
        pixels.data(),
        [&](bool invert_y) {
            // RequestScreenshot fills RGBA8888 in memory order R,G,B,A (Azahar screenshot buffer).
            // Write a PPM (P6, RGB), flipping rows when invert_y so the image is upright.
            std::ofstream f(out_path, std::ios::binary);
            bool ok = static_cast<bool>(f);
            if (ok) {
                f << "P6\n" << layout.width << " " << layout.height << "\n255\n";
                for (u32 y = 0; y < layout.height; y++) {
                    const u32 sy = invert_y ? (layout.height - 1 - y) : y;
                    const u8* row = pixels.data() + static_cast<size_t>(sy) * layout.width * 4;
                    for (u32 x = 0; x < layout.width; x++) {
                        f.put(static_cast<char>(row[x * 4 + 0]));
                        f.put(static_cast<char>(row[x * 4 + 1]));
                        f.put(static_cast<char>(row[x * 4 + 2]));
                    }
                }
            }
            done.set_value(ok);
        },
        layout);

    bool ok = false;
    if (fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        ok = fut.get();
    } else {
        LOG_ERROR(RPC_Server, "Screenshot timed out (no frame rendered?)");
    }
    u32 status = ok ? 1u : 0u;
    std::memcpy(packet.GetPacketData().data(), &status, sizeof(status));
    packet.SetPacketDataSize(sizeof(status));
    packet.SendReply();
}

bool RPCServer::ValidatePacket(const PacketHeader& packet_header) {
    if (packet_header.version <= CURRENT_VERSION) {
        switch (packet_header.packet_type) {
        case PacketType::ReadMemory:
        case PacketType::WriteMemory:
        case PacketType::ProcessList:
        case PacketType::SetGetProcess:
        case PacketType::Screenshot:
            if (packet_header.packet_size >= (sizeof(u32) * 2)) {
                return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

void RPCServer::HandleSingleRequest(std::unique_ptr<Packet> request_packet) {
    bool success = false;
    const auto packet_data = request_packet->GetPacketData();

    if (ValidatePacket(request_packet->GetHeader())) {
        // Currently, all request types use to arguments
        u32 arg1 = 0;
        u32 arg2 = 0;
        std::memcpy(&arg1, packet_data.data(), sizeof(arg1));
        std::memcpy(&arg2, packet_data.data() + sizeof(arg1), sizeof(arg2));

        switch (request_packet->GetPacketType()) {
        case PacketType::ReadMemory:
            if (arg2 > 0 && arg2 <= MAX_READ_SIZE) {
                HandleReadMemory(*request_packet, arg1, arg2);
                success = true;
            }
            break;
        case PacketType::WriteMemory:
            if (arg2 > 0 && arg2 <= MAX_PACKET_DATA_SIZE - (sizeof(u32) * 2)) {
                const auto data = packet_data.subspan(sizeof(u32) * 2, arg2);
                HandleWriteMemory(*request_packet, arg1, data);
                success = true;
            }
            break;
        case PacketType::ProcessList:
            HandleProcessList(*request_packet, arg1, arg2);
            success = true;
            break;
        case PacketType::SetGetProcess:
            HandleSetGetProcess(*request_packet, arg1, arg2);
            success = true;
            break;
        case PacketType::Screenshot:
            // arg1 = res_scale, arg2 = path length; path bytes follow the two u32 args.
            if (arg2 > 0 && arg2 <= MAX_PACKET_DATA_SIZE - (sizeof(u32) * 2)) {
                HandleScreenshot(*request_packet, arg1, packet_data.subspan(sizeof(u32) * 2, arg2));
                success = true;
            }
            break;
        default:
            break;
        }
    }

    if (!success) {
        // Send an empty reply, so as not to hang the client
        request_packet->SetPacketDataSize(0);
        request_packet->SendReply();
    }
}

void RPCServer::HandleRequestsLoop(std::stop_token stop_token) {
    std::unique_ptr<RPC::Packet> request_packet;

    LOG_INFO(RPC_Server, "Request handler started.");

    while ((request_packet = request_queue.PopWait(stop_token))) {
        HandleSingleRequest(std::move(request_packet));
    }
}

void RPCServer::QueueRequest(std::unique_ptr<RPC::Packet> request) {
    request_queue.Push(std::move(request));
}

}; // namespace Core::RPC
