// Authored synthetic fixture; uncompiled/unexecuted at initial Source preparation.
// Any later hosted sanitizer run is separate CI evidence. No game or packfile IO.
#include "../src/client/component/physics_packfile.hpp"

#include <array>
#include <memory>
#include <stdexcept>

namespace
{
	using fastfiles::physics::hk_packfile_header;

	hk_packfile_header valid_header()
	{
		hk_packfile_header header{};
		header.magic[0] = 0x57E0E057;
		header.magic[1] = 0x10C0C010;
		const std::uint8_t layout[4] = { 8, 1, 0, 1 };
		std::memcpy(header.layout_rules, layout, sizeof(layout));
		const char version[] = "hk_2014.2.5-r1";
		std::memcpy(header.contents_version, version, sizeof(version));
		return header;
	}

	void expect_reason(const void* data, const unsigned int size, const char* expected)
	{
		const auto* actual = fastfiles::physics::invalid_havok_data(data, size);
		if ((actual == nullptr) != (expected == nullptr) ||
			(actual && std::strcmp(actual, expected)))
		{
			throw std::runtime_error("Unexpected production physics header validation result");
		}
	}
}

int main()
{
	const auto valid = valid_header();
	static_assert(sizeof(valid) == 64);
	expect_reason(&valid, sizeof(valid), nullptr);

	// Every declared truncation, including valid-looking magic/layout/version.
	// Storage itself is only size bytes; the production guard must read none.
	for (unsigned int size = 0; size < sizeof(valid); ++size)
	{
		const auto prefix = std::make_unique<unsigned char[]>(size);
		std::memcpy(prefix.get(), &valid, size);
		expect_reason(prefix.get(), size, size ? "havok data shorter than packfile header" : "no havok data");
		// A full valid allocation with a truncated declaration must refuse too.
		expect_reason(&valid, size, size ? "havok data shorter than packfile header" : "no havok data");
	}
	for (const auto size : std::array<unsigned int, 5>{ 0u, 1u, 63u, 64u, 80u })
	{
		expect_reason(nullptr, size, "no havok data");
	}
	const struct { hk_packfile_header header; std::array<unsigned char, 16> body; } larger{ valid, {} };
	expect_reason(&larger.header, 65, nullptr);
	expect_reason(&larger.header, sizeof(larger), nullptr);

	alignas(hk_packfile_header) std::array<unsigned char, 65> unaligned{};
	std::memcpy(unaligned.data() + 1, &valid, sizeof(valid));
	expect_reason(unaligned.data() + 1, sizeof(valid), "havok data not 4 byte aligned");
	for (unsigned int word = 0; word < 2; ++word)
	{
		auto changed = valid;
		changed.magic[word] ^= 1;
		expect_reason(&changed, sizeof(changed), "no packfile magic");
	}
	for (unsigned int byte = 0; byte < 4; ++byte)
	{
		auto changed = valid;
		changed.layout_rules[byte] ^= 1;
		expect_reason(&changed, sizeof(changed), "packfile layout differs from the platform's");
	}
	auto changed = valid;
	changed.contents_version[0] = static_cast<char>(0xFF);
	expect_reason(&changed, sizeof(changed), "packfile contents version differs");
	changed = valid;
	changed.contents_version[3] = '0';
	expect_reason(&changed, sizeof(changed), "packfile contents version differs");
	std::memset(changed.contents_version, 'x', sizeof(changed.contents_version));
	expect_reason(&changed, sizeof(changed), "packfile contents version differs");
	changed = valid;
	changed.flags = 1;
	expect_reason(&changed, sizeof(changed), "packfile already loaded");
	changed.flags = 3;
	expect_reason(&changed, sizeof(changed), "packfile already loaded");
	changed.flags = 2;
	expect_reason(&changed, sizeof(changed), nullptr);
}
