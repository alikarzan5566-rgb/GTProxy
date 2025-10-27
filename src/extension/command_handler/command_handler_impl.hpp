#pragma once
#include "command_handler.hpp"
#include "../parser/parser.hpp"
#include "../../core/core.hpp"
#include "../../client/client.hpp"
#include "../../packet/game/core.hpp"
#include "../../packet/packet_types.hpp"
#include "../../packet/packet_variant.hpp"
#include "../../utils/text_parse.hpp"
#include "../../core/logger.hpp"
#include "../../utils/packet_utils.hpp"
#include "../../utils/byte_stream.hpp"
#include "../../server/server.hpp"
#include <magic_enum/magic_enum.hpp>
#include <chrono>
#include <string>
#include <vector>

namespace extension::command_handler {
class CommandHandlerExtension final : public ICommandHandlerExtension {
    core::Core* core_;

    // Toggle states
    bool fly_enabled_ = false;
    bool spam_enabled_ = false;
    bool autofarm_enabled_ = false;

    // Spam configuration
    std::string spam_text_;
    int spam_delay_ = 0;
    std::chrono::steady_clock::time_point spam_last_send_;

    // Auto farm configuration
    uint32_t autofarm_item_id_ = 0;
    bool waiting_for_autofarm_item_selection_ = false;

public:
    explicit CommandHandlerExtension(core::Core* core)
        : core_{ core }
        , spam_last_send_{ std::chrono::steady_clock::now() }
    {
    }

    ~CommandHandlerExtension() override = default;

    std::string create_proxy_help_dialog() {
        std::string dialog =
            "set_default_color|`o\n"
            "add_label_with_icon|big|`wGTProxy Commands|left|9528|\n"
            "add_spacer|small|\n"
            "add_textbox|`2Available Commands:|\n"
            "add_smalltext|`w/proxy `o- Show this help|\n"
            "add_smalltext|`w/fly `o- Toggle flying mode|\n"
            "add_smalltext|`w/spam <text> <delay> `o- Spam messages|\n"
            "add_smalltext|`w/warp <world> `o- Warp to world|\n"
            "add_smalltext|`w/autofarm `o- Auto farm (WIP)|\n"
            "add_spacer|small|\n"
            "add_quick_exit|\n"
            "end_dialog|proxy_help|||";
        return dialog;
    }

    void init() override {
        core_->get_event_dispatcher().prependListener(
            core::EventType::Message,
            [this](const core::EventMessage& event) {
                if (event.from != core::EventFrom::FromClient) {
                    return;
                }

                // Get the text command directly from the already-parsed message
                std::string command = event.get_message().get("text");
                std::string action = event.get_message().get("action");

                // Debug logging
                if (!command.empty() && command[0] == '/') {
                    spdlog::info("Command received: '{}'", command);
                }

                // Handle /proxy
                if (command == "/proxy") {
                    player::Player* to_player = core_->get_server()->get_player();
                    if (to_player) {
                        try {
                            std::string dialog_content = create_proxy_help_dialog();
                            utils::PacketUtils::send_dialog(to_player, dialog_content);
                            spdlog::info("Dialog sent successfully");
                        } catch (const std::exception& e) {
                            spdlog::error("Error sending dialog: {}", e.what());
                            utils::PacketUtils::send_chat_message(to_player, "`4Error showing help dialog");
                        }
                    }
                    event.canceled = true;
                    return;
                }

                // Handle /fly
                if (command == "/fly") {
                    fly_enabled_ = !fly_enabled_;
                    player::Player* to_player = core_->get_server()->get_player();
                    if (to_player) {
                        std::string msg = fly_enabled_ ? "`2Fly enabled!" : "`4Fly disabled!";
                        utils::PacketUtils::send_chat_message(to_player, msg);
                    }
                    event.canceled = true;
                    return;
                }

                // Handle /spam
                if (command.find("/spam") == 0) {
                    std::vector<std::string> tokens = TextParse::tokenize(command, " ");

                    // Toggle off if no args
                    if (tokens.size() == 1) {
                        spam_enabled_ = false;
                        spam_text_.clear();
                        spam_delay_ = 0;

                        player::Player* to_player = core_->get_server()->get_player();
                        if (to_player) {
                            utils::PacketUtils::send_chat_message(to_player, "`4Spam disabled!");
                        }
                        event.canceled = true;
                        return;
                    }

                    // Need at least 3 tokens
                    if (tokens.size() >= 3) {
                        try {
                            std::string delay_str = tokens[tokens.size() - 1];
                            int delay = std::stoi(delay_str);

                            if (delay < 100) {
                                player::Player* to_player = core_->get_server()->get_player();
                                if (to_player) {
                                    utils::PacketUtils::send_chat_message(to_player, "`4Error: Delay must be at least 100ms");
                                }
                                event.canceled = true;
                                return;
                            }

                            // Extract text
                            std::string text;
                            for (size_t i = 1; i < tokens.size() - 1; i++) {
                                if (i > 1) text += " ";
                                text += tokens[i];
                            }

                            spam_enabled_ = true;
                            spam_text_ = text;
                            spam_delay_ = delay;
                            spam_last_send_ = std::chrono::steady_clock::now();

                            player::Player* to_player = core_->get_server()->get_player();
                            if (to_player) {
                                utils::PacketUtils::send_chat_message(to_player,
                                    "`2Spam enabled! Message: '" + text + "' every " + std::to_string(delay) + "ms");
                            }
                        } catch (...) {
                            player::Player* to_player = core_->get_server()->get_player();
                            if (to_player) {
                                utils::PacketUtils::send_chat_message(to_player, "`4Error: Invalid spam command");
                            }
                        }
                    } else {
                        player::Player* to_player = core_->get_server()->get_player();
                        if (to_player) {
                            utils::PacketUtils::send_chat_message(to_player, "`4Usage: /spam <text> <delay_ms>");
                        }
                    }
                    event.canceled = true;
                    return;
                }

                // Handle /warp
                if (command.find("/warp ") == 0) {
                    std::vector<std::string> tokens = TextParse::tokenize(command, " ");
                    if (tokens.size() >= 2) {
                        std::string worldname = tokens[1];

                        player::Player* server_player = core_->get_client()->get_player();
                        if (server_player && server_player->is_connected()) {
                            try {
                                TextParse message;
                                message.add("action", {"join_request"});
                                message.add("name", {worldname});
                                message.add("invitedWorld", {"0"});

                                std::string raw_message = message.get_raw();

                                ByteStream<> byte_stream;
                                byte_stream.write(packet::NET_MESSAGE_GENERIC_TEXT);
                                byte_stream.write(raw_message, false);

                                server_player->send_packet(byte_stream.get_data(), 0);

                                player::Player* to_player = core_->get_server()->get_player();
                                if (to_player) {
                                    utils::PacketUtils::send_chat_message(to_player, "`2Warping to " + worldname + "...");
                                }
                            } catch (const std::exception& e) {
                                spdlog::error("Error sending warp: {}", e.what());
                            }
                        }
                    } else {
                        player::Player* to_player = core_->get_server()->get_player();
                        if (to_player) {
                            utils::PacketUtils::send_chat_message(to_player, "`4Usage: /warp <worldname>");
                        }
                    }
                    event.canceled = true;
                    return;
                }
            }
        );

        // Packet listener for fly
        core_->get_event_dispatcher().prependListener(
            core::EventType::Packet,
            [this](const core::EventPacket& event) {
                if (event.from != core::EventFrom::FromClient) {
                    return;
                }

                packet::GameUpdatePacket packet = event.get_packet();

                // Only handle fly if enabled and it's a state packet
                if (fly_enabled_ && packet.type == packet::PACKET_STATE) {
                    // Don't modify, just let it through for now
                    // Fly modification can be tricky and cause crashes
                    // We'll implement it more carefully later
                }
            }
        );

        // Disconnect handler
        core_->get_event_dispatcher().prependListener(
            core::EventType::Disconnection,
            [this](const core::EventDisconnection& event) {
                if (event.from == core::EventFrom::FromClient) {
                    fly_enabled_ = false;
                    spam_enabled_ = false;
                    spam_text_.clear();
                    spam_delay_ = 0;
                    autofarm_enabled_ = false;
                    autofarm_item_id_ = 0;
                    waiting_for_autofarm_item_selection_ = false;
                }
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
                player::Player* server_player = core_->get_client()->get_player();
                if (server_player && server_player->is_connected()) {
                    try {
                        TextParse message;
                        message.add("action", {"input"});
                        message.add("text", {spam_text_});

                        std::string raw_message = message.get_raw();

                        ByteStream<> byte_stream;
                        byte_stream.write(packet::NET_MESSAGE_GENERIC_TEXT);
                        byte_stream.write(raw_message, false);

                        server_player->send_packet(byte_stream.get_data(), 0);
                        spam_last_send_ = now;
                    } catch (const std::exception& e) {
                        spdlog::error("Error sending spam message: {}", e.what());
                    }
                }
            }
        }
    }

    void free() override {
        delete this;
    }
};
}
