"""Source checks only: the authored C++ fixture is not compiled or executed."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BASE = 'cfca45b11ee14641945b9a264a95638387f48a4a'
PATH = 'src/client/component/fastfiles.cpp'
original = subprocess.check_output(['git', '-C', str(ROOT), 'show', f'{BASE}:{PATH}'], text=True)
current = (ROOT / PATH).read_text()
helper = (ROOT / 'src/client/component/physics_packfile.hpp').read_text()
database = (ROOT / 'src/client/game/types/database.hpp').read_text()


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def compact(source):
    return re.sub(r'\s+', '', source)


asset = block(database, 'struct PhysicsAsset\n')
assert re.search(r'\bunsigned int\s+havokDataSize\s*;', asset)
assert re.search(r'\bchar\*\s+havokData\s*;', asset)
old_struct = block(original, 'struct hk_packfile_header')
assert compact(old_struct) == compact(block(helper, 'struct hk_packfile_header'))
assert 'static_assert(sizeof(hk_packfile_header) == 64);' in helper
for field, offset in [('layout_rules', 16), ('contents_version', 40), ('flags', 56)]:
    assert f'static_assert(offsetof(hk_packfile_header, {field}) == {offset});' in helper
old_function = block(original, 'const char* invalid_havok_data(')
body = block(helper, 'inline const char* invalid_havok_data(')
guard = block(body, 'if (havok_data_size < sizeof(hk_packfile_header))')
assert compact(guard) == compact('if (havok_data_size < sizeof(hk_packfile_header)) { return "havok data shorter than packfile header"; }')
assert body.index(guard) < body.index('reinterpret_cast') < body.index('header->')
old_body = old_function[old_function.index('{'):].replace('asset->havokDataSize', 'havok_data_size').replace('asset->havokData', 'havok_data')
new_body = body[body.index('{'):].replace(guard, '')
assert compact(old_body) == compact(new_body)
wrapper = block(current, 'const char* invalid_havok_data(')
assert compact(wrapper[wrapper.index('{'):]) == compact('{ return physics::invalid_havok_data(asset->havokData, asset->havokDataSize); }')
old_region = original[original.index('struct hk_packfile_header'):original.index(old_function) + len(old_function)]
expected = original.replace(old_region, wrapper).replace('#include "fastfiles.hpp"', '#include "fastfiles.hpp"\n#include "physics_packfile.hpp"')
assert current == expected
assert 'const unsigned int havok_data_size' in body
print('Source checks passed: unsigned field type, unchanged layout/reasons, early bounds and exact production delegation; C++ fixture AUTHORED UNCOMPILED UNEXECUTED.')
