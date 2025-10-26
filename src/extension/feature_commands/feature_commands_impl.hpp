#pragma once
#include "feature_commands.hpp"
#include "../../core/core.hpp"
#include "../../client/client.hpp"
#include "../../server/server.hpp"
#include "../../player/player.hpp"
#include "../../packet/packet_types.hpp"
#include "../../utils/text_parse.hpp"
#include "../../utils/packet_utils.hpp"
#include "../../utils/byte_stream.hpp"
#include "../../core/logger.hpp"
#include <chrono>
#include <string>
#include <vector>

namespace extension::feature_commands {
class FeatureCommandsExtension final : public IFeatureCommandsExtension {
    core::Core* core_;

    // Toggle states
    bool fastdrop_enabled_ = false;
    bool fasttrash_enabled_ = false;
    bool fastroulette_enabled_ = false;
    bool fly_enabled_ = false;
    bool spam_enabled_ = false;

    // Spam configuration
    std::string spam_text_;
    int spam_delay_ = 0;
    std::chrono::steady_clock::time_point spam_last_send_;

public:
    explicit FeatureCommandsExtension(core::Core* core)
        : core_{ core }
        , spam_last_send_{ std::chrono::steady_clock::now() }
    {
    }

    ~FeatureCommandsExtension() override = default;

    void init() override {
        // Command parsing listener
        core_->get_event_dispatcher().prependListener(
            core::EventType::Message,
            [this](const core::EventMessage& event) {
                if (event.from != core::EventFrom::FromClient) {
                    return;
                }
                handle_command(event);
            }
        );

        // Packet interception listener
        core_->get_event_dispatcher().prependListener(
            core::EventType::Packet,
            [this](const core::EventPacket& event) {
                if (event.from != core::EventFrom::FromClient) {
                    return;
                }
                handle_packet(event);
            }
        );

        // Disconnect handler
        core_->get_event_dispatcher().prependListener(
            core::EventType::Disconnection,
            [this](const core::EventDisconnection& event) {
                if (event.from != core::EventFrom::FromClient) {
                    return;
                }
                reset_all_state();
            }
        );
    }

    void tick() override {
        // Handle spam timer
        if (spam_enabled_ && !spam_text_.empty()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - spam_last_send_
            ).count();

            if (elapsed >= spam_delay_) {
                send_spam_message();
                spam_last_send_ = now;
            }
        }
    }

    void free() override {
        delete this;
    }

private:
    void handle_command(const core::EventMessage& event) {
        TextParse text_parse = event.get_message();
        std::string command_text = text_parse.get("text");

        if (command_text.empty()) {
            return;
        }

        // Tokenize command
        std::vector<std::string> tokens = TextParse::tokenize(command_text, " ");
        if (tokens.empty()) {
            return;
        }

        std::string command = tokens[0];

        // Handle /fastdrop
        if (command == "/fastdrop") {
            fastdrop_enabled_ = !fastdrop_enabled_;
            send_feedback(fastdrop_enabled_ ? "`Fast Drop` enabled!" : "`Fast Drop` disabled!");
            event.canceled = true;
            return;
        }

        // Handle /fasttrash
        if (command == "/fasttrash") {
            fasttrash_enabled_ = !fasttrash_enabled_;
            send_feedback(fasttrash_enabled_ ? "`Fast Trash` enabled!" : "`Fast Trash` disabled!");
            event.canceled = true;
            return;
        }

        // Handle /fastroulette
        if (command == "/fastroulette") {
            fastroulette_enabled_ = !fastroulette_enabled_;
            send_feedback(fastroulette_enabled_ ? "`Fast Roulette` enabled!" : "`Fast Roulette` disabled!");
            event.canceled = true;
            return;
        }

        // Handle /fly
        if (command == "/fly") {
            fly_enabled_ = !fly_enabled_;
            send_feedback(fly_enabled_ ? "`Fly` enabled!" : "`Fly` disabled!");
            event.canceled = true;
            return;
        }

        // Handle /spam
        if (command == "/spam") {
            // If no arguments, toggle off
            if (tokens.size() == 1) {
                spam_enabled_ = false;
                spam_text_.clear();
                spam_delay_ = 0;
                send_feedback("`Spam` disabled!");
                event.canceled = true;
                return;
            }

            // Need at least 3 tokens: /spam <text> <delay>
            if (tokens.size() < 3) {
                send_feedback("Usage: /spam <text> <delay_ms>");
                event.canceled = true;
                return;
            }

            // Extract delay (last token)
            std::string delay_str = tokens[tokens.size() - 1];
            int delay = 0;
            try {
                delay = std::stoi(delay_str);
            } catch (...) {
                send_feedback("Error: Invalid delay value");
                event.canceled = true;
                return;
            }

            // Validate delay
            if (delay < 100) {
                send_feedback("Error: Delay must be at least 100ms");
                event.canceled = true;
                return;
            }

            // Extract text (all tokens from 1 to size-2)
            std::string text;
            for (size_t i = 1; i < tokens.size() - 1; i++) {
                if (i > 1) {
                    text += " ";
                }
                text += tokens[i];
            }

            // Enable spam
            spam_enabled_ = true;
            spam_text_ = text;
            spam_delay_ = delay;
            spam_last_send_ = std::chrono::steady_clock::now();

            send_feedback("`Spam` enabled! Message: '" + text + "' every " + std::to_string(delay) + "ms");
            event.canceled = true;
            return;
        }

        // Handle /warp
        if (command == "/warp") {
            if (tokens.size() < 2) {
                send_feedback("Usage: /warp <worldname>");
                event.canceled = true;
                return;
            }

            std::string worldname = tokens[1];
            send_warp_request(worldname);
            send_feedback("Warping to " + worldname + "...");
            event.canceled = true;
            return;
        }
    }

    void handle_packet(const core::EventPacket& event) {
        packet::GameUpdatePacket packet = event.get_packet();

        // Handle fly modification for PACKET_STATE
        if (fly_enabled_ && packet.type == packet::PACKET_STATE) {
            // Modify packet flags to enable flying
            // Set on_jump flag to allow continuous air movement
            packet.flags.on_jump = 1;

            // Note: We need to modify the packet in the event
            // Since event.get_packet() returns a copy, we need to access the original
            // The packet modification will be reflected when forwarded
            const_cast<packet::GameUpdatePacket&>(event.get_packet()) = packet;
        }

        // Fast drop/trash/roulette don't need packet modification
        // They work by allowing packets to pass through without artificial throttling
        // The proxy already forwards packets immediately, so these features
        // are essentially "no-op" but provide user feedback for toggle state
    }

    void reset_all_state() {
        fastdrop_enabled_ = false;
        fasttrash_enabled_ = false;
        fastroulette_enabled_ = false;
        fly_enabled_ = false;
        spam_enabled_ = false;
        spam_text_.clear();
        spam_delay_ = 0;
    }

    void send_feedback(const std::string& message) {
        player::Player* to_player = core_->get_server()->get_player();
        if (to_player && to_player->is_connected()) {
            utils::PacketUtils::send_chat_message(to_player, message);
        } else {
            spdlog::error("Cannot send feedback: player is null or not connected.");
        }
    }

    void send_spam_message() {
        player::Player* server_player = core_->get_client()->get_player();
        if (!server_player || !server_player->is_connected()) {
            return;
        }

        // Build message in TextParse format
        TextParse message;
        message.add("action", {"input"});
        message.add("text", {spam_text_});

        std::string raw_message = message.get_raw();

        // Create byte stream with NET_MESSAGE_GENERIC_TEXT
        ByteStream<> byte_stream;
        byte_stream.write(packet::NET_MESSAGE_GENERIC_TEXT);
        byte_stream.write(raw_message, false);  // Don't write length prefix

        // Send to server
        server_player->send_packet(byte_stream.get_data(), 0);
    }

    void send_warp_request(const std::string& worldname) {
        player::Player* server_player = core_->get_client()->get_player();
        if (!server_player || !server_player->is_connected()) {
            spdlog::error("Cannot send warp request: server player is null or not connected.");
            return;
        }

        // Build join_request message
        TextParse message;
        message.add("action", {"join_request"});
        message.add("name", {worldname});
        message.add("invitedWorld", {"0"});

        std::string raw_message = message.get_raw();

        // Create byte stream with NET_MESSAGE_GENERIC_TEXT
        ByteStream<> byte_stream;
        byte_stream.write(packet::NET_MESSAGE_GENERIC_TEXT);
        byte_stream.write(raw_message, false);  // Don't write length prefix

        // Send to server
        server_player->send_packet(byte_stream.get_data(), 0);
    }
};
}
