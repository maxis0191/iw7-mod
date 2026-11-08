#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "download_cache.hpp"
#include "console/console.hpp"
#include "scheduler.hpp"

#include "game/game.hpp"

#include <utils/io.hpp>
#include <utils/json.hpp>
#include <utils/string.hpp>

// Automatic downloads (sv_wwwBaseUrl) write mods and usermaps into the game folder and nothing ever removes them.
// Each downloaded folder gets a small record of the files the download wrote and when the folder was last used.
// At startup the client deletes those files from folders unused for cl_downloadCacheDays days.
// Folders without a record (installed by hand) and files the download did not write are never touched.

namespace download_cache
{
	namespace
	{
		constexpr auto record_name = "download_cache.json";
		constexpr auto seconds_per_day = 24ll * 60ll * 60ll;

		const game::dvar_t* cl_download_cache_days = nullptr;
		std::mutex record_mutex;

		std::string normalize(std::string path)
		{
			std::ranges::replace(path, '\\', '/');
			while (path.ends_with('/'))
			{
				path.pop_back();
			}

			return utils::string::to_lower(path);
		}

		bool is_safe_name(const std::string& name)
		{
			return !name.empty() && name != "." && name != ".." && !name.contains('/') &&
				!name.contains('\\') && !name.contains(':');
		}

		// accepts exactly "mods/<name>" or "usermaps/<name>"
		bool is_cache_folder(const std::string& folder)
		{
			const auto separator = folder.find('/');
			if (separator == std::string::npos)
			{
				return false;
			}

			const auto root = folder.substr(0, separator);
			return (root == "mods" || root == "usermaps") && is_safe_name(folder.substr(separator + 1));
		}

		std::int64_t now()
		{
			return static_cast<std::int64_t>(std::time(nullptr));
		}

		std::string get_record_path(const std::string& folder)
		{
			return folder + "/" + record_name;
		}

		std::optional<nlohmann::json> read_record(const std::string& folder)
		{
			std::string data;
			if (!utils::io::read_file(get_record_path(folder), &data))
			{
				return {};
			}

			try
			{
				auto record = nlohmann::json::parse(data);
				if (!record.is_object() || !record["files"].is_array() || !record["last_used"].is_number_integer())
				{
					return {};
				}

				return {std::move(record)};
			}
			catch (const std::exception&)
			{
				return {};
			}
		}

		void write_record(const std::string& folder, const nlohmann::json& record)
		{
			utils::io::write_file(get_record_path(folder), record.dump(1, '\t'), false);
		}

		std::string get_current_fs_game()
		{
			const auto* fs_game = game::Dvar_FindVar("fs_game");
			if (fs_game == nullptr || fs_game->current.string == nullptr)
			{
				return {};
			}

			return normalize(fs_game->current.string);
		}

		// returns true when the folder's downloaded files are gone and the record was removed
		bool remove_downloaded_files(const std::string& folder, const nlohmann::json& record)
		{
			auto removed_all = true;
			for (const auto& entry : record["files"])
			{
				if (!entry.is_string() || !is_safe_name(entry.get<std::string>()))
				{
					continue;
				}

				const auto path = folder + "/" + entry.get<std::string>();
				std::error_code ec{};
				if (std::filesystem::exists(path, ec) && !std::filesystem::remove(path, ec))
				{
					console::warn("[download cache] could not delete %s (%s)\n", path.data(), ec.message().data());
					removed_all = false;
				}
			}

			if (!removed_all)
			{
				return false;
			}

			std::error_code ec{};
			std::filesystem::remove(get_record_path(folder), ec);
			if (std::filesystem::is_empty(folder, ec) && !ec)
			{
				std::filesystem::remove(folder, ec);
			}

			return true;
		}

		void clean_root(const std::string& root, const std::int64_t max_age, const std::string& in_use)
		{
			std::error_code ec{};
			if (!std::filesystem::is_directory(root, ec))
			{
				return;
			}

			std::vector<std::string> folders{};
			for (const auto& entry : std::filesystem::directory_iterator(root, ec))
			{
				// plain directories only: links and junctions are left alone
				std::error_code entry_ec{};
				if (entry.symlink_status(entry_ec).type() != std::filesystem::file_type::directory)
				{
					continue;
				}

				const auto name = entry.path().filename().u8string();
				folders.emplace_back(normalize(root + "/" + std::string(name.begin(), name.end())));
			}

			for (const auto& folder : folders)
			{
				if (!is_cache_folder(folder) || folder == in_use)
				{
					continue;
				}

				std::lock_guard _(record_mutex);
				const auto record = read_record(folder);
				if (!record)
				{
					continue;
				}

				const auto last_used = (*record)["last_used"].get<std::int64_t>();
				const auto age = now() - last_used;
				if (age < max_age || last_used > now())
				{
					continue;
				}

				if (remove_downloaded_files(folder, *record))
				{
					console::info("[download cache] deleted %s, unused for %lld days\n", folder.data(),
						static_cast<long long>(age / seconds_per_day));
				}
			}
		}

		void clean_unused()
		{
			const auto days = cl_download_cache_days ? cl_download_cache_days->current.integer : 0;
			if (days <= 0)
			{
				return;
			}

			const auto in_use = get_current_fs_game();
			scheduler::once([days, in_use]
			{
				try
				{
					const auto max_age = static_cast<std::int64_t>(days) * seconds_per_day;
					clean_root("mods", max_age, in_use);
					clean_root("usermaps", max_age, in_use);
				}
				catch (const std::exception& e)
				{
					console::warn("[download cache] cleanup failed: %s\n", e.what());
				}
			}, scheduler::pipeline::async);
		}
	}

	void record_download(const std::string& file)
	{
		const auto path = normalize(file);
		const auto separator = path.find_last_of('/');
		if (separator == std::string::npos)
		{
			return;
		}

		const auto folder = path.substr(0, separator);
		const auto name = path.substr(separator + 1);
		if (!is_cache_folder(folder) || !is_safe_name(name) || name == record_name)
		{
			return;
		}

		std::lock_guard _(record_mutex);
		auto record = read_record(folder).value_or(nlohmann::json{{"files", nlohmann::json::array()}});

		auto& files = record["files"];
		if (std::ranges::find(files, nlohmann::json(name)) == files.end())
		{
			files.push_back(name);
		}

		record["last_used"] = now();
		write_record(folder, record);
	}

	void mark_used(const std::string& folder_)
	{
		const auto folder = normalize(folder_);
		if (!is_cache_folder(folder))
		{
			return;
		}

		std::lock_guard _(record_mutex);
		auto record = read_record(folder);
		if (!record)
		{
			return;
		}

		(*record)["last_used"] = now();
		write_record(folder, *record);
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

			cl_download_cache_days = game::Dvar_RegisterInt("cl_downloadCacheDays", 30, 0, 3650,
				game::DVAR_FLAG_SAVED, "Delete automatically downloaded mods and usermaps unused for this many days (0 = never)");

			// wait for the saved config so a changed cl_downloadCacheDays applies
			scheduler::once(clean_unused, scheduler::pipeline::main, 10s);
		}
	};
}

REGISTER_COMPONENT(download_cache::component)
