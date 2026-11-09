#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "network.hpp"
#include "scheduler.hpp"

#include <utils/hook.hpp>

namespace voice
{
	namespace
	{
		constexpr auto max_players = 18;
		constexpr auto max_frames = 10;
		constexpr auto max_frame_size = 254;

		constexpr auto max_packet_size = 1200;

		constexpr auto talker_bytes_per_second = 3000;
		constexpr auto packet_cost = 64;

		constexpr auto cs_player_info = 0xF1A;

		utils::hook::detour cl_voice_flush_hook;

		game::dvar_t* sv_voice = nullptr;

		bool choosing_recipients = false;

		std::uint64_t registered_xuids[max_players]{};

		struct talker_budget
		{
			game::netadr_s address{};
			std::uint32_t time{};
			int millibytes{};
		};

		talker_budget talker_budgets[max_players]{};

		struct address_reference
		{
			void* const* vftable;
			volatile long ref_count;
		};

		void release_address_reference(address_reference*, int)
		{
		}

		void* const address_reference_vftable[] = { reinterpret_cast<void*>(release_address_reference) };
		address_reference relay_address_reference{ address_reference_vftable, 0x40000000 };

		bool read_frames(std::string_view data, std::vector<std::string_view>& frames)
		{
			if (data.empty())
			{
				return false;
			}

			const auto count = static_cast<std::uint8_t>(data[0]);
			data.remove_prefix(1);

			if (count < 1 || count > max_frames)
			{
				return false;
			}

			for (auto i = 0; i < count; i++)
			{
				if (data.empty())
				{
					return false;
				}

				const auto size = static_cast<std::uint8_t>(data[0]);
				if (size < 1 || size > max_frame_size || size > data.size() - 1)
				{
					return false;
				}

				frames.emplace_back(data.substr(1, size));
				data.remove_prefix(1 + size);
			}

			return data.empty();
		}

		void write_frames(std::string& data, const std::vector<std::string_view>& frames)
		{
			data.push_back(static_cast<char>(frames.size()));
			for (const auto& frame : frames)
			{
				data.push_back(static_cast<char>(frame.size()));
				data.append(frame);
			}
		}

		game::SessionData* get_session()
		{
			return game::g_serverSession.get();
		}

		int get_local_talker(game::SessionData* session)
		{
			return game::Voice_GetLocalTalker(game::CL_ControllerIndexFromClientNum(0), session);
		}

		bool is_server_running()
		{
			return (game::environment::is_dedi() || game::Com_IsAnyLocalServerRunning()) && game::SV_Loaded()
				&& !game::Com_FrontEndScene_IsActive();
		}

		bool is_relaying()
		{
			return sv_voice && sv_voice->current.enabled && is_server_running();
		}

		bool is_in_game()
		{
			return game::clientUIActives[0].connectionState == game::CA_ACTIVE && !game::Com_FrontEndScene_IsActive();
		}

		const game::netadr_s* get_server_address()
		{
			const auto* connection = *game::cl_con_data;
			return connection && connection->address.type == game::NA_IP ? &connection->address : nullptr;
		}

		bool is_relay_active()
		{
			if (game::environment::is_dedi() || !is_in_game())
			{
				return false;
			}

			if (game::Com_IsAnyLocalServerRunning())
			{
				return is_relaying();
			}

			return get_server_address() != nullptr;
		}

		void play_frames(const int talker, const std::vector<std::string_view>& frames)
		{
			auto* session = get_session();
			if (talker == get_local_talker(session) || !game::Session_IsUserRegistered(session, talker)
				|| game::CL_IsPlayerMuted(session, talker))
			{
				return;
			}

			const auto* cl_voice = game::Dvar_FindVar("cl_voice");
			if (!cl_voice || !cl_voice->current.enabled)
			{
				return;
			}

			for (const auto& frame : frames)
			{
				game::Voice_IncomingVoiceData(session, talker, frame.data(), static_cast<int>(frame.size()), 0);
			}
		}

		const game::client_t* get_active_client(const int client_num)
		{
			if (client_num < 0 || client_num >= std::min(static_cast<int>(*game::svs_numclients), max_players))
			{
				return nullptr;
			}

			const auto* client = game::svs_clients[client_num];
			return client && client->header.state == game::CS_ACTIVE ? client : nullptr;
		}

		int find_client(const game::netadr_s& address)
		{
			for (auto i = 0; i < max_players; i++)
			{
				const auto* client = get_active_client(i);
				if (client && network::are_addresses_equal(client->remoteAddress, address))
				{
					return i;
				}
			}

			return -1;
		}

		bool take_budget(const int talker, const game::netadr_s& address, const size_t size)
		{
			constexpr auto capacity = talker_bytes_per_second * 1000;

			auto& budget = talker_budgets[talker];
			const auto now = static_cast<std::uint32_t>(game::Sys_Milliseconds());

			if (!network::are_addresses_equal(budget.address, address))
			{
				budget = { address, now, capacity };
			}

			const auto elapsed = static_cast<int>(std::min(now - budget.time, 1000u));
			budget.time = now;
			budget.millibytes = std::min(capacity, budget.millibytes + elapsed * talker_bytes_per_second);

			const auto cost = (static_cast<int>(size) + packet_cost) * 1000;
			if (budget.millibytes < cost)
			{
				return false;
			}

			budget.millibytes -= cost;
			return true;
		}

		void relay_frames(const int talker, const std::uint32_t recipients, const std::vector<std::string_view>& frames)
		{
			std::string data;
			data.push_back(static_cast<char>(talker));
			write_frames(data, frames);

			for (auto i = 0; i < max_players; i++)
			{
				const auto* client = get_active_client(i);
				if (!client || i == talker || !(recipients & (1u << i)))
				{
					continue;
				}

				if (client->remoteAddress.type == game::NA_LOOPBACK)
				{
					play_frames(talker, frames);
				}
				else if (client->remoteAddress.type == game::NA_IP)
				{
					network::send(client->remoteAddress, "voiceData", data);
				}
			}
		}

		void receive_from_client(const game::netadr_s& address, const std::string_view& data)
		{
			if (!is_relaying() || data.size() < sizeof(std::uint32_t) || data.size() > max_packet_size)
			{
				return;
			}

			const auto talker = find_client(address);
			if (talker < 0)
			{
				return;
			}

			std::uint32_t recipients{};
			std::memcpy(&recipients, data.data(), sizeof(recipients));

			std::vector<std::string_view> frames{};
			if (!read_frames(data.substr(sizeof(recipients)), frames) || !take_budget(talker, address, data.size()))
			{
				return;
			}

			relay_frames(talker, recipients, frames);
		}

		void receive_from_server(const game::netadr_s& address, const std::string_view& data)
		{
			const auto* server = get_server_address();
			if (!is_relay_active() || !server || !network::are_addresses_equal(*server, address)
				|| data.empty() || data.size() > max_packet_size)
			{
				return;
			}

			const auto talker = static_cast<std::uint8_t>(data[0]);
			std::vector<std::string_view> frames{};
			if (talker >= max_players || !read_frames(data.substr(1), frames))
			{
				return;
			}

			play_frames(talker, frames);
		}

		void send_frames(const std::uint32_t recipients, const std::vector<std::string_view>& frames)
		{
			if (!game::Com_IsAnyLocalServerRunning())
			{
				std::string data;
				data.append(reinterpret_cast<const char*>(&recipients), sizeof(recipients));
				write_frames(data, frames);
				network::send(*get_server_address(), "voice", data);
				return;
			}

			for (auto i = 0; i < max_players; i++)
			{
				const auto* client = get_active_client(i);
				if (client && client->remoteAddress.type == game::NA_LOOPBACK)
				{
					relay_frames(i, recipients, frames);
					return;
				}
			}
		}

		std::uint32_t get_recipients(game::SessionData* session, const int local_client_num, const int talker)
		{
			choosing_recipients = true;

			std::uint32_t recipients{};
			for (auto i = 0; i < max_players; i++)
			{
				if (i != talker && game::Voice_ShareChannel(talker, i)
					&& game::CL_ShouldSendVoiceTo(session, local_client_num, i))
				{
					recipients |= 1u << i;
				}
			}

			choosing_recipients = false;
			return recipients;
		}

		void cl_voice_flush_stub(const int local_client_num)
		{
			if (!is_relay_active())
			{
				cl_voice_flush_hook.invoke<void>(local_client_num);
				return;
			}

			auto* session = get_session();
			const auto talker = get_local_talker(session);
			const auto count = std::min(*game::cl_voicePacketCount, max_frames);
			if (talker < 0 || count <= 0)
			{
				return;
			}

			const auto* winvoice_loopback = game::Dvar_FindVar("winvoice_loopback");

			std::vector<std::string_view> frames{};
			for (auto i = 0; i < count; i++)
			{
				const auto& packet = game::cl_voicePackets[i];
				if (packet.dataSize < 1 || packet.dataSize > max_frame_size)
				{
					continue;
				}

				frames.emplace_back(packet.data, packet.dataSize);

				if (winvoice_loopback && winvoice_loopback->current.enabled)
				{
					game::Voice_IncomingVoiceData(session, talker, packet.data, packet.dataSize, local_client_num);
				}
			}

			const auto recipients = get_recipients(session, local_client_num, talker);
			if (!recipients || frames.empty())
			{
				return;
			}

			auto size = sizeof(recipients) + 1;
			std::vector<std::string_view> packet{};
			for (const auto& frame : frames)
			{
				if (size + 1 + frame.size() > max_packet_size)
				{
					send_frames(recipients, packet);
					packet.clear();
					size = sizeof(recipients) + 1;
				}

				packet.emplace_back(frame);
				size += 1 + frame.size();
			}

			send_frames(recipients, packet);
		}

		bool is_loopback_stub(game::SessionData* session, const int client_num)
		{
			return !choosing_recipients && utils::hook::invoke<bool>(0x140C732A0, session, client_num);
		}

		void** get_address_reference_stub(void** reference, const int handle)
		{
			if (!choosing_recipients)
			{
				return utils::hook::invoke<void**>(0x140D92790, reference, handle);
			}

			_InterlockedIncrement(&relay_address_reference.ref_count);
			*reference = &relay_address_reference;
			return reference;
		}

		void cl_voice_receive_stub(const int local_client_num, game::msg_t* msg)
		{
			if (!is_relay_active())
			{
				utils::hook::invoke<void>(0x14037CFF0, local_client_num, msg);
			}
		}

		std::uint64_t get_player_xuid(const int client_num)
		{
			const std::string info = game::CL_GetConfigString(cs_player_info + client_num);
			const auto xuid_string = info.substr(0, info.find(' '));

			std::uint64_t xuid{};
			if (xuid_string.empty() || !game::StringToXUID(xuid_string.data(), &xuid))
			{
				return 0;
			}

			return xuid;
		}

		bool is_registered(game::SessionData* session, const std::uint64_t xuid)
		{
			for (const auto& user : session->dyn.users)
			{
				if (user.registered && user.xuid == xuid)
				{
					return true;
				}
			}

			return false;
		}

		void release_talkers(game::SessionData* session)
		{
			for (auto i = 0; i < max_players; i++)
			{
				if (registered_xuids[i] && session->dyn.users[i].registered && session->dyn.users[i].xuid == registered_xuids[i])
				{
					game::Session_UnregisterRemotePlayer(session, i);
				}

				registered_xuids[i] = 0;
			}
		}

		void register_talkers()
		{
			auto* session = get_session();
			if (!session->dyn.sessionHandle)
			{
				std::memset(registered_xuids, 0, sizeof(registered_xuids));
				return;
			}

			if (!is_relay_active())
			{
				if (game::clientUIActives[0].connectionState < game::CA_CONNECTED || game::Com_FrontEndScene_IsActive())
				{
					release_talkers(session);
				}

				return;
			}

			const auto local_talker = get_local_talker(session);
			if (local_talker < 0)
			{
				return;
			}

			game::netadr_s address{};
			address.type = game::NA_LOOPBACK;

			for (auto i = 0; i < max_players; i++)
			{
				const auto& user = session->dyn.users[i];
				const auto xuid = i == local_talker ? 0 : get_player_xuid(i);

				if (registered_xuids[i] && (!user.registered || user.xuid != registered_xuids[i]))
				{
					registered_xuids[i] = 0;
				}

				if (registered_xuids[i] && registered_xuids[i] != xuid)
				{
					game::Session_UnregisterRemotePlayer(session, i);
					registered_xuids[i] = 0;
				}

				if (!xuid || user.registered || is_registered(session, xuid))
				{
					continue;
				}

				std::array<char, 0x25> xnaddr{};
				game::Session_RegisterRemotePlayer(game::CL_ControllerIndexFromClientNum(0), session, xuid, false, i, 0,
					xnaddr.data(), &address);

				if (user.registered && user.xuid == xuid)
				{
					registered_xuids[i] = xuid;
				}
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			network::on("voice", receive_from_client);

			scheduler::once([]()
			{
				sv_voice = game::Dvar_RegisterBool("sv_voice", true, game::DVAR_FLAG_NONE, "Relay voice chat between players");
			}, scheduler::pipeline::main);

			if (game::environment::is_dedi())
			{
				return;
			}

			network::on("voiceData", receive_from_server);

			cl_voice_flush_hook.create(0x14037D5C0, cl_voice_flush_stub);
			utils::hook::call(0x1409B0714, cl_voice_receive_stub);

			utils::hook::call(0x14099E2DE, is_loopback_stub);
			utils::hook::call(0x14099E349, get_address_reference_stub);

			scheduler::loop(register_talkers, scheduler::pipeline::main, 250ms);
		}
	};
}

REGISTER_COMPONENT(voice::component)
