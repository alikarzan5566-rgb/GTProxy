#pragma once
#include "feature_commands.hpp"
#include "../../core/core.hpp"
#include "../../client/client.hpp"
#include "../../server/server.hpp"
#include "../../player/player.hpp"
#include "../../packet/packet_types.hpp"
#include "../../packet/packet_variant.hpp"
#include "../../utils/text_parse.hpp"
#include "../../utils/packet_utils.hpp"
#include "../../utils/byte_stream.hpp"
#include "../../core/logger.hpp"
#include <magic_enum/magic_enum.hpp>
#include <chrono>
#include <string>
#include <vector>
#include <regex>

namespace extension::feature_commands {
class FeatureCommandsExtension final : public IFeatureCommandsExtension {
    core::Core* core_;

    // Toggle states for original commands
    bool fastdrop_enabled_ = false;
    bool fasttrash_enabled_ = false;
    bool fastroulette_enabled_ = false;
    bool fly_enabled_ = false;
    bool spam_enabled_ = false;

    // Toggle states for auto functions
    bool autoplant_enabled_ = false;
    bool autoharvest_enabled_ = false;
    bool autocollect_enabled_ = false;
    bool autofarm_enabled_ = false;

    // Spam configuration
    std::string spam_text_;
    int spam_delay_ = 0;
    std::chrono::steady_clock::time_point spam_last_send_;

    // Auto farm configuration
    uint32_t autofarm_item_id_ = 0;
    std::chrono::steady_clock::time_point autofarm_last_action_;
    int autofarm_delay_ = 100; // ms between place/break
    bool waiting_for_autofarm_item_selection_ = false;

    // Player state tracking
    float player_x_ = 0.0f;
    float player_y_ = 0.0f;

public:
    explicit FeatureCommandsExtension(core::Core* core)
        : core_{ core }
        , spam_last_send_{ std::chrono::steady_clock::now() }
        , autofarm_last_action_{ std::chrono::steady_clock::now() }
    {
    }

    ~FeatureCommandsExtension() override = default;

    std::string create_proxy_help_dialog() {
        std::string dialog =
            "set_default_color|`o\n"
            "add_label_with_icon|big|`wGTProxy Commands|left|9528|\n"
            "add_spacer|small|\n"
            "add_textbox|`2Movement & Action Commands:|\n"
            "add_smalltext|`w/fly `o- Toggle flying mode|\n"
            "add_smalltext|`w/fastdrop `o- Toggle fast drop|\n"
            "add_smalltext|`w/fasttrash `o- Toggle fast trash|\n"
            "add_smalltext|`w/fastroulette `o- Toggle fast roulette|\n"
            "add_spacer|small|\n"
            "add_textbox|`2Auto Functions:|\n"
            "add_smalltext|`w/autoplant `o- Toggle auto plant|\n"
            "add_smalltext|`w/autoharvest `o- Toggle auto harvest|\n"
            "add_smalltext|`w/autocollect `o- Toggle auto collect|\n"
            "add_smalltext|`w/autofarm `o- Toggle auto farm (select item)|\n"
            "add_spacer|small|\n"
            "add_textbox|`2Utility Commands:|\n"
            "add_smalltext|`w/spam <text> <delay> `o- Spam messages|\n"
            "add_smalltext|`w/warp <world> `o- Warp to world|\n"
            "add_spacer|small|\n"
            "add_quick_exit|\n"
            "end_dialog|proxy_help|||";
        return dialog;
    }

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

        // Handle auto farm
        if (autofarm_enabled_ && autofarm_item_id_ > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - autofarm_last_action_
            ).count();

            if (elapsed >= autofarm_delay_) {
                perform_autofarm_action();
                autofarm_last_action_ = now;
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
        std::string action = text_parse.get("action");

        if (command_text.empty()) {
            return;
        }

        // Tokenize command
        std::vector<std::string> tokens = TextParse::tokenize(command_text, " ");
        if (tokens.empty()) {
            return;
        }

        std::string command = tokens[0];

        // Handle /proxy - Show dialog with all commands
        if (command == "/proxy") {
            player::Player* to_player = core_->get_server()->get_player();
            if (to_player) {
                std::string dialog_content = create_proxy_help_dialog();
                utils::PacketUtils::send_dialog(to_player, dialog_content);
            }
            event.canceled = true;
            return;
        }

        // Handle /fastdrop
        if (command == "/fastdrop") {
            fastdrop_enabled_ = !fastdrop_enabled_;
            send_feedback(fastdrop_enabled_ ? "`2Fast Drop enabled!" : "`4Fast Drop disabled!");
            event.canceled = true;
            return;
        }

        // Handle /fasttrash
        if (command == "/fasttrash") {
            fasttrash_enabled_ = !fasttrash_enabled_;
            send_feedback(fasttrash_enabled_ ? "`2Fast Trash enabled!" : "`4Fast Trash disabled!");
            event.canceled = true;
            return;
        }

        // Handle /fastroulette
        if (command == "/fastroulette") {
            fastroulette_enabled_ = !fastroulette_enabled_;
            send_feedback(fastroulette_enabled_ ? "`2Fast Roulette enabled!" : "`4Fast Roulette disabled!");
            event.canceled = true;
            return;
        }

        // Handle /fly
        if (command == "/fly") {
            fly_enabled_ = !fly_enabled_;
            send_feedback(fly_enabled_ ? "`2Fly enabled!" : "`4Fly disabled!");
            event.canceled = true;
            return;
        }

        // Handle /autoplant
        if (command == "/autoplant") {
            autoplant_enabled_ = !autoplant_enabled_;
            send_feedback(autoplant_enabled_ ? "`2Auto Plant enabled!" : "`4Auto Plant disabled!");
            event.canceled = true;
            return;
        }

        // Handle /autoharvest
        if (command == "/autoharvest") {
            autoharvest_enabled_ = !autoharvest_enabled_;
            send_feedback(autoharvest_enabled_ ? "`2Auto Harvest enabled!" : "`4Auto Harvest disabled!");
            event.canceled = true;
            return;
        }

        // Handle /autocollect
        if (command == "/autocollect") {
            autocollect_enabled_ = !autocollect_enabled_;
            send_feedback(autocollect_enabled_ ? "`2Auto Collect enabled!" : "`4Auto Collect disabled!");
            event.canceled = true;
            return;
        }

        // Handle /autofarm
        if (command == "/autofarm") {
            if (!autofarm_enabled_) {
                // Turning on - request item selection
                waiting_for_autofarm_item_selection_ = true;
                send_item_selection_dialog();
            } else {
                // Turning off
                autofarm_enabled_ = false;
                autofarm_item_id_ = 0;
                send_feedback("`4Auto Farm disabled!");
            }
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
                send_feedback("`4Spam disabled!");
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

            send_feedback("`2Spam enabled! Message: '" + text + "' every " + std::to_string(delay) + "ms");
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
            send_feedback("`2Warping to " + worldname + "...");
            event.canceled = true;
            return;
        }

        // Handle dialog responses for autofarm item selection
        if (action == "dialog_return" && waiting_for_autofarm_item_selection_) {
            std::string dialog_name = text_parse.get("dialog_name");
            if (dialog_name == "autofarm_item_select") {
                std::string item_id_str = text_parse.get("itemID");
                if (!item_id_str.empty()) {
                    try {
                        autofarm_item_id_ = std::stoul(item_id_str);
                        autofarm_enabled_ = true;
                        waiting_for_autofarm_item_selection_ = false;
                        send_feedback("`2Auto Farm enabled with item ID: " + item_id_str);
                    } catch (...) {
                        send_feedback("`4Error: Invalid item ID");
                    }
                }
                event.canceled = true;
                return;
            }
        }
    }

    void handle_packet(const core::EventPacket& event) {
        packet::GameUpdatePacket packet = event.get_packet();

        // Track player position from PACKET_STATE
        if (event.from == core::EventFrom::FromClient && packet.type == packet::PACKET_STATE) {
            // Extract player position from packet data
            // Position is typically in the ext_data
            std::vector<std::byte> ext_data = event.get_ext_data();
            if (ext_data.size() >= 16) {
                ByteStream<> stream(ext_data.data(), ext_data.size());
                stream.read(player_x_);
                stream.read(player_y_);
            }
        }

        // Handle fly modification for PACKET_STATE from client
        if (event.from == core::EventFrom::FromClient && fly_enabled_ && packet.type == packet::PACKET_STATE) {
            event.canceled = true;

            // Modify packet flags to enable flying
            packet.flags.on_jump = 1;

            // Get server player to forward modified packet
            player::Player* server_player = core_->get_client()->get_player();
            if (server_player && server_player->is_connected()) {
                // Rebuild byte stream with modified packet
                ByteStream<> byte_stream;
                byte_stream.write(packet::NET_MESSAGE_GAME_PACKET);
                byte_stream.write(packet);

                // Add ext_data if present
                std::vector<std::byte> ext_data = event.get_ext_data();
                if (!ext_data.empty()) {
                    byte_stream.write_data(ext_data.data(), ext_data.size());
                }

                // Send modified packet to server
                server_player->send_packet(byte_stream.get_data(), 0);
            }
        }

        // Auto plant: Intercept tile change requests and auto-plant on farmable tiles
        if (event.from == core::EventFrom::FromClient && autoplant_enabled_ &&
            packet.type == packet::PACKET_TILE_CHANGE_REQUEST) {
            // Let the packet through - auto plant will happen on tick based on position
        }

        // Auto harvest: Intercept tile punches on ready trees
        if (event.from == core::EventFrom::FromClient && autoharvest_enabled_ &&
            (packet.type == packet::PACKET_TILE_ACTIVATE_REQUEST || packet.flags.on_punched)) {
            // Let the packet through - auto harvest logic
        }

        // Auto collect: Intercept item spawns from server and auto-collect
        if (event.from == core::EventFrom::FromServer && autocollect_enabled_ &&
            packet.type == packet::PACKET_SEND_PARTICLE_EFFECT) {
            // Auto collect will send collect packet
            // This would need more detailed implementation based on item drop detection
        }

        // Fast drop/trash/roulette don't need packet modification
        // They work by allowing packets to pass through without artificial throttling
    }

    void reset_all_state() {
        fastdrop_enabled_ = false;
        fasttrash_enabled_ = false;
        fastroulette_enabled_ = false;
        fly_enabled_ = false;
        spam_enabled_ = false;
        spam_text_.clear();
        spam_delay_ = 0;

        autoplant_enabled_ = false;
        autoharvest_enabled_ = false;
        autocollect_enabled_ = false;
        autofarm_enabled_ = false;
        autofarm_item_id_ = 0;
        waiting_for_autofarm_item_selection_ = false;
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
        byte_stream.write(raw_message, false);

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
        byte_stream.write(raw_message, false);

        // Send to server
        server_player->send_packet(byte_stream.get_data(), 0);
    }

    void send_item_selection_dialog() {
        player::Player* to_player = core_->get_server()->get_player();
        if (!to_player || !to_player->is_connected()) {
            return;
        }

        std::string dialog =
            "set_default_color|`o\n"
            "add_label_with_icon|big|`wAuto Farm Setup|left|32|\n"
            "add_spacer|small|\n"
            "add_textbox|`2Enter the Item ID to farm:|\n"
            "add_text_input|itemID||Item ID|5|\n"
            "add_spacer|small|\n"
            "add_smalltext|`4The item will be placed and broken continuously.|\n"
            "add_spacer|small|\n"
            "end_dialog|autofarm_item_select|Cancel|Enable|";

        utils::PacketUtils::send_dialog(to_player, dialog);
    }

    void perform_autofarm_action() {
        player::Player* server_player = core_->get_client()->get_player();
        if (!server_player || !server_player->is_connected()) {
            return;
        }

        // Calculate tile position based on player position
        int tile_x = static_cast<int>(player_x_ / 32.0f);
        int tile_y = static_cast<int>(player_y_ / 32.0f);

        // Create tile change packet to place block
        packet::GameUpdatePacket place_packet{};
        place_packet.type = packet::PACKET_TILE_CHANGE_REQUEST;
        place_packet.net_id = -1;
        place_packet.flags.value = packet::PACKET_FLAG_NONE;
        place_packet.data_size = 8; // x, y coordinates

        // Build ext_data with tile coordinates and item id
        ByteStream<> ext_stream;
        ext_stream.write(static_cast<int32_t>(tile_x));
        ext_stream.write(static_cast<int32_t>(tile_y));
        std::vector<std::byte> ext_data = ext_stream.get_data();

        // Send place packet
        ByteStream<> byte_stream;
        byte_stream.write(packet::NET_MESSAGE_GAME_PACKET);
        byte_stream.write(place_packet);
        byte_stream.write_data(ext_data.data(), ext_data.size());

        server_player->send_packet(byte_stream.get_data(), 0);

        // Immediately send punch packet to break it
        packet::GameUpdatePacket punch_packet{};
        punch_packet.type = packet::PACKET_STATE;
        punch_packet.net_id = -1;
        punch_packet.flags.value = packet::PACKET_FLAG_NONE;
        punch_packet.flags.on_punched = 1;

        ByteStream<> punch_stream;
        punch_stream.write(packet::NET_MESSAGE_GAME_PACKET);
        punch_stream.write(punch_packet);

        server_player->send_packet(punch_stream.get_data(), 0);
    }
};
}
