#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fastfiles::physics
{
	struct hk_packfile_header
	{
		std::uint32_t magic[2];
		std::int32_t user_tag;
		std::int32_t file_version;
		std::uint8_t layout_rules[4];
		std::int32_t num_sections;
		std::int32_t contents_section_index;
		std::int32_t contents_section_offset;
		std::int32_t contents_class_name_section_index;
		std::int32_t contents_class_name_section_offset;
		char contents_version[16];
		std::uint32_t flags;
		std::int32_t pad;
	};
	static_assert(sizeof(hk_packfile_header) == 64);
	static_assert(offsetof(hk_packfile_header, layout_rules) == 16);
	static_assert(offsetof(hk_packfile_header, contents_version) == 40);
	static_assert(offsetof(hk_packfile_header, flags) == 56);

	// PhysicsAsset::havokDataSize is unsigned int in game/types/database.hpp.
	// This validates only the existing fixed header, never the packfile body.
	inline const char* invalid_havok_data(const void* havok_data, const unsigned int havok_data_size)
	{
		if (!havok_data || !havok_data_size)
		{
			return "no havok data";
		}

		if (havok_data_size < sizeof(hk_packfile_header))
		{
			return "havok data shorter than packfile header";
		}

		if (reinterpret_cast<std::uintptr_t>(havok_data) & 3)
		{
			return "havok data not 4 byte aligned";
		}

		const auto* header = reinterpret_cast<const hk_packfile_header*>(havok_data);
		if (header->magic[0] != 0x57E0E057 || header->magic[1] != 0x10C0C010)
		{
			return "no packfile magic";
		}

		const std::uint8_t layout[4] = { 8, 1, 0, 1 };
		if (std::memcmp(header->layout_rules, layout, sizeof(layout)))
		{
			return "packfile layout differs from the platform's";
		}

		if (static_cast<std::uint8_t>(header->contents_version[0]) == 0xFF ||
			std::strncmp(header->contents_version, "hk_2014.2.5-r1", sizeof(header->contents_version)))
		{
			return "packfile contents version differs";
		}

		if (header->flags & 1)
		{
			return "packfile already loaded";
		}

		return nullptr;
	}
}
