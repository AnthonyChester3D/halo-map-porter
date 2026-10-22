# Generates tag_layouts.inc from Invader's tag definitions (invader/src/tag/hek/definition/*.json):
#   python3 gen_tag_layouts.py <definition folder> tag_layouts.inc
# The app reads a few kinds of tag itself (structure BSPs, models, scenarios, objects, bitmaps, shaders) instead
# of asking invader-edit for each value: these are their structs' sizes and fields, and every struct they contain.
import glob, json, os, sys

ROOTS = {   # tag extension -> root struct
    'scenario_structure_bsp': 'ScenarioStructureBSP', 'model': 'Model', 'gbxmodel': 'GBXModel', 'scenario': 'Scenario',
    'biped': 'Biped', 'vehicle': 'Vehicle', 'weapon': 'Weapon', 'equipment': 'Equipment', 'garbage': 'Garbage',
    'projectile': 'Projectile', 'scenery': 'Scenery', 'device_machine': 'DeviceMachine', 'device_control': 'DeviceControl',
    'device_light_fixture': 'DeviceLightFixture', 'placeholder': 'Placeholder', 'sound_scenery': 'SoundScenery',
    'item_collection': 'ItemCollection', 'bitmap': 'Bitmap', 'shader_model': 'ShaderModel',
    'shader_environment': 'ShaderEnvironment', 'shader_transparent_chicago': 'ShaderTransparentChicago',
    'shader_transparent_chicago_extended': 'ShaderTransparentChicagoExtended',
    'shader_transparent_generic': 'ShaderTransparentGeneric', 'effect': 'Effect', 'light': 'Light', 'sky': 'Sky',
}

types = {}
for p in sorted(glob.glob(os.path.join(sys.argv[1], '*.json'))):
    for x in json.load(open(p)):
        types[x['name']] = x

PRIM = {  # type -> (type char, bytes per value, values)
    'float': ('f', 4, 1), 'Angle': ('f', 4, 1), 'Fraction': ('f', 4, 1), 'int8': ('b', 1, 1), 'uint8': ('B', 1, 1),
    'int16': ('h', 2, 1), 'uint16': ('H', 2, 1), 'Index': ('H', 2, 1), 'int32': ('i', 4, 1), 'uint32': ('I', 4, 1),
    'TagID': ('I', 4, 1), 'TagFourCC': ('I', 4, 1), 'Pointer': ('I', 4, 1), 'ColorARGBInt': ('I', 4, 1),
    'Point2DInt': ('h', 2, 2), 'Rectangle2D': ('h', 2, 4), 'ColorARGB': ('f', 4, 4), 'ColorRGB': ('f', 4, 3),
    'Point2D': ('f', 4, 2), 'Point3D': ('f', 4, 3), 'Vector2D': ('f', 4, 2), 'Vector3D': ('f', 4, 3),
    'Euler2D': ('f', 4, 2), 'Euler3D': ('f', 4, 3), 'Quaternion': ('f', 4, 4), 'Plane2D': ('f', 4, 3),
    'Plane3D': ('f', 4, 4), 'Matrix': ('f', 4, 9),
}


def fields_of(s):
    return (fields_of(types[s['inherits']]) if 'inherits' in s else []) + s['fields']


def size_of(f):
    t = f['type']
    n = f.get('count', 1) * (2 if f.get('bounds') else 1)
    if t == 'pad': return f['size']
    if t in PRIM: return PRIM[t][1] * PRIM[t][2] * n
    if t == 'TagString': return 32 * n
    if t == 'TagDependency': return 16 * n
    if t == 'TagReflexive': return 12 * n
    if t == 'TagDataOffset': return 20 * n
    d = types[t]
    if d['type'] == 'enum': return 2 * n
    if d['type'] == 'bitfield': return d['width'] // 8 * n
    return struct_size(d) * n


def struct_size(s):
    return sum(size_of(f) for f in fields_of(s))


order, index = [], {}


def visit(name):
    if name in index: return index[name]
    index[name] = len(order)
    order.append(name)
    for f in fields_of(types[name]):
        t = f['type']
        if t == 'TagReflexive': visit(f['struct'])
        elif t in types and types[t]['type'] == 'struct': visit(t)
    return index[name]


for root in ROOTS.values(): visit(root)

out_fields, out_structs = [], []
for name in order:
    s = types[name]
    first, off = len(out_fields), 0
    for f in fields_of(s):
        t, sz = f['type'], size_of(f)
        n = f.get('count', 1) * (2 if f.get('bounds') else 1)
        fname = f.get('name', '')
        kind, ch, sub, count = None, 'x', -1, n
        if t == 'pad': pass
        elif t == 'TagDependency': kind = 1
        elif t == 'TagReflexive': kind, sub = 2, index[f['struct']]
        elif t == 'TagDataOffset': kind = 3
        elif t in PRIM: kind, ch, count = 0, PRIM[t][0], PRIM[t][2] * n
        elif t == 'TagString': kind, ch = 0, 's'
        elif types[t]['type'] == 'enum': kind, ch = 0, 'H'
        elif types[t]['type'] == 'bitfield': kind, ch = 0, {8: 'B', 16: 'H', 32: 'I'}[types[t]['width']]
        else: kind, sub = 4, index[t]
        # A field the reader needs: anything with data after the struct, or a named value.
        if kind is not None and (kind != 0 or fname):
            out_fields.append('    {"%s", %d, %d, \'%s\', %d, %d},' % (fname, off, kind, ch, sub, count))
        off += sz
    if s.get('size') is not None and s['size'] != off:
        sys.exit('%s: size %d, fields add up to %d' % (name, s['size'], off))
    out_structs.append('    {"%s", %d, %d, %d},' % (name, off, first, len(out_fields) - first))

lines = [
    '// Halo CE tag (HEK) layouts the app reads directly: structure BSPs, models, scenarios, objects.',
    "// Generated from Invader's tag definitions (src/tag/hek/definition/*.json) by gen_tag_layouts.py.",
    '// kind: 0 value (type: f float, b/B 8-bit, h/H 16-bit, i/I 32-bit, s 32-character string; count values),',
    '// 1 tag reference (16 bytes, its path after the struct), 2 block (12 bytes, its elements after the struct;',
    '// sub = their struct), 3 data (20 bytes, its bytes after the struct), 4 struct inside the struct (sub).',
    'struct TagLayoutField { const char *name; unsigned short offset; unsigned char kind; char type; short sub; unsigned short count; };',
    'struct TagLayoutStruct { const char *name; unsigned size; unsigned short first, fields; };',
    'struct TagLayoutRoot { const char *extension, *root; };',
    'static const TagLayoutField kTagFields[] = {'] + out_fields + [
    '};',
    'static const TagLayoutStruct kTagStructs[] = {'] + out_structs + [
    '};',
    'static const TagLayoutRoot kTagRoots[] = {'] + ['    {"%s", "%s"},' % kv for kv in ROOTS.items()] + [
    '};']
open(sys.argv[2], 'w', newline='\n').write('\n'.join(lines) + '\n')
print('%d structs, %d fields' % (len(out_structs), len(out_fields)))
