#pragma once
#include "../packet/message/chat.hpp"
#include "../packet/packet_helper.hpp"
#include "../packet/packet_variant.hpp"
#include "../player/player.hpp"
#include "../core/logger.hpp"
#include "../utils/byte_stream.hpp"
#include <magic_enum/magic_enum.hpp>

namespace utils {
class PacketUtils {
public:
    static void send_chat_message(player::Player* player, const std::string& message) {
        if (!player || !player->is_connected()) {
            spdlog::error("Cannot send message: player is null or not connected.");
            return;
        }

        packet::message::Log message_packet{};
        message_packet.msg = message;

        if (!packet::PacketHelper::send(message_packet, *player)) {
            spdlog::error("Failed to send chat message packet to player.");
        }
    }

    static void send_dialog(player::Player* player, const std::string& dialog_content) {
        if (!player || !player->is_connected()) {
            spdlog::error("Cannot send dialog: player is null or not connected.");
            return;
        }

        // Create variant with OnDialogRequest function call
        packet::Variant variant("OnDialogRequest", dialog_content);
        std::vector<std::byte> variant_data = variant.serialize();

        // Create game packet
        packet::GameUpdatePacket packet{};
        std::memset(&packet, 0, sizeof(packet));
        packet.type = packet::PACKET_CALL_FUNCTION;
        packet.net_id = -1;
        packet.flags.value = packet::PACKET_FLAG_NONE;
        packet.data_size = static_cast<uint32_t>(variant_data.size());
        packet.decompressed_data_size = 0;

        // Build byte stream
        ByteStream<> byte_stream;
        byte_stream.write(magic_enum::enum_underlying(packet::NET_MESSAGE_GAME_PACKET));
        byte_stream.write(packet);
        byte_stream.write_data(variant_data.data(), variant_data.size());

        if (!player->send_packet(byte_stream.get_data(), 0)) {
            spdlog::error("Failed to send dialog packet to player.");
        }
    }
};
}