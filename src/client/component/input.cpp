#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "game_console.hpp"

#include "gui/gui.hpp"

#include "input.hpp"
#include "scheduler.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>

namespace input
{
	namespace
	{
		std::vector<std::string> custom_binds = {};

		utils::hook::detour cl_char_event_hook;
		utils::hook::detour cl_key_event_hook;
		utils::hook::detour cl_execute_key_hook;
		
		utils::hook::detour cl_mouse_move_hook;

		int key_get_binding_for_cmd_stub(const char* command);

		enum class menu_movement_state
		{
			off,
			armed,
			active,
		};

		constexpr auto ui_key_catcher = 0x40;
		constexpr auto menu_movement_arm_time = 2000ms;

		std::atomic<menu_movement_state> menu_movement = menu_movement_state::off;
		std::atomic<std::chrono::steady_clock::time_point> menu_movement_armed_at{};
		int menu_movement_held[256]{};
		thread_local bool in_cmd_input_check = false;

		std::vector<int> get_bindings_for_cmds(const std::initializer_list<const char*>& commands)
		{
			std::vector<int> bindings{};
			for (const auto* command : commands)
			{
				bindings.emplace_back(key_get_binding_for_cmd_stub(command));
			}

			return bindings;
		}

		bool is_menu_movement_hold(const int binding)
		{
			static const auto bindings = get_bindings_for_cmds({ "+forward", "+back", "+moveleft", "+moveright",
				"+breath_sprint", "+sprint", "+gostand", "+stance", "+movedown", "+togglecrouch", "+prone", "+slide" });
			return binding > 0 && std::ranges::find(bindings, binding) != bindings.end();
		}

		bool is_menu_movement_press(const int binding)
		{
			static const auto bindings = get_bindings_for_cmds({ "gocrouch", "goprone", "toggleprone" });
			return binding > 0 && std::ranges::find(bindings, binding) != bindings.end();
		}

		void update_menu_movement()
		{
			const auto ui_open = (*game::keyCatchers & ui_key_catcher) != 0;
			const auto state = menu_movement.load();

			if (state == menu_movement_state::armed)
			{
				if (ui_open)
				{
					menu_movement = menu_movement_state::active;
				}
				else if (std::chrono::steady_clock::now() - menu_movement_armed_at.load() > menu_movement_arm_time)
				{
					menu_movement = menu_movement_state::off;
				}
			}
			else if (state == menu_movement_state::active && !ui_open)
			{
				menu_movement = menu_movement_state::off;
			}
		}

		void exec_binding(const int local_client_num, const int binding, const int key)
		{
			utils::hook::invoke<void>(0x14035DFB0, local_client_num, binding, key, 1); // CL_InputMP_ExecBinding
		}

		bool is_menu_movement_active()
		{
			return menu_movement == menu_movement_state::active && *game::keyCatchers == ui_key_catcher;
		}

		bool cl_cmd_input_allowed_stub(const int local_client_num)
		{
			in_cmd_input_check = true;
			const auto result = utils::hook::invoke<bool>(0x14023E6F0, local_client_num);
			in_cmd_input_check = false;
			return result;
		}

		bool cmd_key_is_catcher_active_stub(const int local_client_num, const int mask)
		{
			if (in_cmd_input_check && local_client_num == 0 && is_menu_movement_active())
			{
				return false;
			}

			return utils::hook::invoke<bool>(0x1409A9250, local_client_num, mask); // Key_IsCatcherActive
		}

		bool menu_movement_key_event(const int local_client_num, const int key, const int down)
		{
			if (local_client_num != 0 || key < 0 || key >= 256)
			{
				return false;
			}

			update_menu_movement();
			const auto in_menu = is_menu_movement_active();

			if (!down && menu_movement_held[key])
			{
				exec_binding(local_client_num, menu_movement_held[key] + 1, key);
				menu_movement_held[key] = 0;
				return in_menu;
			}

			if (!in_menu)
			{
				return false;
			}

			const auto binding = game::playerKeys->keys[key].binding;
			if (is_menu_movement_hold(binding))
			{
				if (!down)
				{
					exec_binding(local_client_num, binding + 1, key);
				}
				else if (!menu_movement_held[key])
				{
					exec_binding(local_client_num, binding, key);
					menu_movement_held[key] = binding;
				}

				return true;
			}

			if (is_menu_movement_press(binding))
			{
				if (down)
				{
					exec_binding(local_client_num, binding, key);
				}

				return true;
			}

			return false;
		}

		void cl_char_event_stub(const int local_client_num, const int key)
		{
			if (!game_console::console_char_event(local_client_num, key))
			{
				return;
			}
			
			if (!gui::gui_char_event(local_client_num, key))
			{
				return;
			}

			cl_char_event_hook.invoke<void>(local_client_num, key);
		}

		void cl_key_event_stub(const int local_client_num, const int key, const int down)
		{
			if (!game_console::console_key_event(local_client_num, key, down))
			{
				return;
			}
			
			if (!gui::gui_key_event(local_client_num, key, down))
			{
				return;
			}

			if (menu_movement_key_event(local_client_num, key, down))
			{
				return;
			}

			cl_key_event_hook.invoke<void>(local_client_num, key, down);

			const auto key_catchers = *game::keyCatchers;
			if (key_catchers != 0)
			{
				return;
			}

			auto binding = game::playerKeys->keys[key].binding;
			static const auto binding1 = key_get_binding_for_cmd_stub("+actionslot 8");
			if (binding == binding1)
			{
				if (!down)
				{
					binding = binding + 1;
				}
				utils::hook::invoke<void>(0x14035DFB0, local_client_num, binding, key, 1); // CL_InputMP_ExecBinding
			}
		}
		
		void cl_mouse_move_stub(const int local_client_num, int x, int y)
		{
			if (!gui::gui_mouse_event(local_client_num, x, y))
			{
				return;
			}

			cl_mouse_move_hook.invoke<void>(local_client_num, x, y);
		}

		int get_num_keys()
		{
			return 118;
		}

		int key_write_bindings_to_buffer_stub(int, char* buffer, const int buffer_size)
		{
			auto bytes_used = 0;
			const auto buffer_size_align = static_cast<std::int32_t>(buffer_size) - 4;

			for (auto key_index = 0; key_index < 256; ++key_index)
			{
				const auto* const key_button = game::Key_KeynumToString(key_index, 0, 1);
				auto value = game::playerKeys->keys[key_index].binding;

				if (value && value < get_num_keys())
				{
					const auto len = sprintf_s(&buffer[bytes_used], (buffer_size_align - bytes_used),
						"bind %s \"%s\"\n", key_button, game::command_whitelist[value]);

					if (len < 0)
					{
						return bytes_used;
					}

					bytes_used += len;
				}
				else if (value >= get_num_keys())
				{
					value -= get_num_keys();
					if (static_cast<size_t>(value) < custom_binds.size() && !custom_binds[value].empty())
					{
						const auto len = sprintf_s(&buffer[bytes_used], (buffer_size_align - bytes_used),
							"bind %s \"%s\"\n", key_button, custom_binds[value].data());

						if (len < 0)
						{
							return bytes_used;
						}

						bytes_used += len;
					}
				}
			}

			buffer[bytes_used] = 0;
			return bytes_used;
		}

		int get_binding_for_custom_command(const char* command)
		{
			auto index = 0;
			for (auto& bind : custom_binds)
			{
				if (bind == command)
				{
					return index;
				}
				index++;
			}

			custom_binds.emplace_back(command);
			index = static_cast<unsigned int>(custom_binds.size()) - 1;

			return index;
		}

		std::optional<std::string*> get_custom_binding_for_key(int key)
		{
			key -= get_num_keys();

			if (static_cast<size_t>(key) < custom_binds.size() && !custom_binds[key].empty())
			{
				return { &custom_binds[key] };
			}

			return {};
		}

		int key_get_binding_for_cmd_stub(const char* command)
		{
			// original binds
			for (auto i = 0; i < get_num_keys(); i++)
			{
				if (game::command_whitelist[i] && !strcmp(command, game::command_whitelist[i]))
				{
					return i;
				}
			}

			// custom binds
			return get_num_keys() + get_binding_for_custom_command(command);
		}

		const char* key_get_cmd_for_binding_stub(int localClientNum, int keyBinding)
		{
			if (keyBinding >= get_num_keys())
			{
				const auto bind = get_custom_binding_for_key(keyBinding);
				if (bind.has_value())
				{
					return bind.value()->data();
				}
				return "";
			}

			return game::command_whitelist[keyBinding];
		}

		void cl_execute_key_stub(const int local_client_num, int key, const int down, const unsigned int time)
		{
			if (key >= get_num_keys())
			{
				const auto bind = get_custom_binding_for_key(key);
				if (!bind.has_value())
				{
					return;
				}

				return game::Cbuf_AddText(local_client_num, utils::string::va("%s\n", bind.value()->data()));
			}

			cl_execute_key_hook.invoke<void>(local_client_num, key, down, time);
		}
	}

	void set_menu_movement(const bool enabled)
	{
		if (enabled)
		{
			menu_movement_armed_at = std::chrono::steady_clock::now();
			menu_movement = (*game::keyCatchers & ui_key_catcher) ? menu_movement_state::active : menu_movement_state::armed;
		}
		else
		{
			menu_movement = menu_movement_state::off;
		}
	}

	bool get_menu_movement()
	{
		return menu_movement != menu_movement_state::off;
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			cl_char_event_hook.create(0x1409A7350, cl_char_event_stub);
			cl_key_event_hook.create(0x1409A7980, cl_key_event_stub);
			cl_mouse_move_hook.create(0x140615A50, cl_mouse_move_stub);

			custom_binds.push_back("+actionslot 8");
			custom_binds.push_back("-actionslot 8");

			// write all bindings to config file
			utils::hook::jump(0x1409A9F70, key_write_bindings_to_buffer_stub);

			// links a custom command to an index
			utils::hook::jump(0x1409A8EA0, key_get_binding_for_cmd_stub);
			utils::hook::jump(0x1409A91D0, key_get_cmd_for_binding_stub);

			// execute custom binds
			cl_execute_key_hook.create(0x14032A3B0, &cl_execute_key_stub);

			scheduler::loop(update_menu_movement, scheduler::pipeline::main);

			// the usercmd builder drops all key input while any key catcher is up: let it through for the menu that asked
			utils::hook::call(0x14036DC77, cl_cmd_input_allowed_stub);
			utils::hook::call(0x14023F720, cmd_key_is_catcher_active_stub);
		}
	};
}

REGISTER_COMPONENT(input::component)