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

At initial Source preparation (`d510986`),
`tests/physics_packfile_header_test.cpp` was **AUTHORED, UNCOMPILED, UNEXECUTED**.
It calls the production helper with authored valid 64-byte headers, allocated
prefixes and declared truncations 0..63, 65/80-byte cases, null and unaligned
pointers, both magic words, all layout bytes, invalid versions and loaded flags.
No stock bytes, packfile body parsing or runtime evidence is involved.

`python -B tests/physics_packfile_source_test.py` passed Source extraction and
guard ordering against the exact base during that preparation. The script never
compiles or executes C++ and cannot establish absence of a native crash or
successful gameplay. No local build, SDK probe, installed binary change, package,
lease, game or publication is part of this Source preparation.

The first Source comparison detected an incidental final newline added to
`fastfiles.cpp`; restoring the base file's ending made the exact extraction
check pass. No C++ result is inferred from this Source-only correction.

The dedicated `physics-packfile-source.yml` workflow prepares later hosted
validation on `ubuntu-24.04`, using the existing checkout action version with
full baseline history, Source-only sparse paths, LFS and submodules disabled.
Its finite steps run the Source checker, compile this same standalone fixture
as C++17 with g++/ASan/UBSan, and execute it with fail-fast sanitizers. Each
command has a timeout and the whole job has a five-minute limit. It installs
nothing and neither builds nor launches IW7. The old native build workflow is
unchanged. This workflow has not been published or triggered during preparation;
Root exact-Source approval and a separate remote-CI/push GO precede activation.
Any subsequent CI evidence must identify its exact commit and run separately
from the initial uncompiled status and from native/gameplay validation.
