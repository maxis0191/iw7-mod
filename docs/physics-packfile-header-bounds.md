# Physics packfile header bounds

Source base: `cfca45b11ee14641945b9a264a95638387f48a4a`.

The existing `invalid_havok_data` reads a fixed header after checking only a
nonzero size and pointer alignment. Positive declared sizes below the complete
64-byte header therefore reach header reads. Public source declares
`PhysicsAsset::havokDataSize` as `unsigned int` and `havokData` as `char*` in
`src/client/game/types/database.hpp`; no signed-size or new engine ABI is assumed.

The existing header and validation move mechanically into the standard-only
`physics_packfile.hpp`. The production asset wrapper calls that same helper, so
the standalone fixture exercises production logic without loading game types,
hooks or an SDK. A new `size < sizeof(hk_packfile_header)` refusal occurs before
every cast or header access. Null/zero retains `no havok data`; other truncated
inputs receive `havok data shorter than packfile header`. Full-size alignment,
magic, layout, bounded version and loaded-flag checks retain their old order,
reason strings and accepted cases. This still validates only the fixed header.

`tests/physics_packfile_header_test.cpp` is **AUTHORED, UNCOMPILED, UNEXECUTED**.
It calls the production helper with authored valid 64-byte headers, allocated
prefixes and declared truncations 0..63, 65/80-byte cases, null and unaligned
pointers, both magic words, all layout bytes, invalid versions and loaded flags.
No stock bytes, packfile body parsing or runtime evidence is involved.

`python -B tests/physics_packfile_source_test.py` passed Source extraction and
guard ordering against the exact base. It does not compile or execute C++ and
cannot establish absence of a native crash or successful gameplay. No build,
SDK probe, installed binary change, package, lease, game, publication or CI is
part of this local Source candidate. Root and qualification review the exact
commit before deciding any later validation or publication.

The first Source comparison detected an incidental final newline added to
`fastfiles.cpp`; restoring the base file's ending made the exact extraction
check pass. No C++ result is inferred from this Source-only correction.
