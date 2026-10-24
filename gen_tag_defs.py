# Generates tag_defs.inc from Invader's tag definitions (invader/src/tag/hek/definition/*.json):
#   python3 gen_tag_defs.py <definition folder> tag_defs.inc
# Every tag kind's structs as Invader's tools see them (tagedit.cpp): each field's key name, where it is, what kind of
# value it holds, whether Invader lists it, lets it be edited, or leaves it out when it saves a tag; enum and bitfield
# names; the classes a reference takes; the root struct and header version of each tag class.
import glob, json, os, sys

defs = {}
for p in sorted(glob.glob(os.path.join(sys.argv[1], '*.json'))):
    for x in json.load(open(p)):
        defs[x['name']] = x

# Value types, numbered as Invader's ParserStructValue::ValueType (tagedit.hpp keeps the same order).
VT = ['INT8', 'UINT8', 'INT16', 'UINT16', 'INDEX', 'INT32', 'UINT32', 'COLORARGBINT', 'POINT2DINT', 'RECTANGLE2D',
      'FLOAT', 'FRACTION', 'ANGLE', 'COLORARGB', 'COLORRGB', 'VECTOR2D', 'VECTOR3D', 'EULER2D', 'EULER3D', 'PLANE2D',
      'PLANE3D', 'POINT2D', 'POINT3D', 'QUATERNION', 'MATRIX', 'REFLEXIVE', 'DEPENDENCY', 'TAGID', 'TAGSTRING',
      'TAGDATAOFFSET', 'ENUM', 'BITMASK']
VALUE = {   # type -> (value type, bytes of one element, bytes per component)
    'int8': ('INT8', 1, 1), 'uint8': ('UINT8', 1, 1), 'int16': ('INT16', 2, 2), 'uint16': ('UINT16', 2, 2),
    'Index': ('INDEX', 2, 2), 'int32': ('INT32', 4, 4), 'uint32': ('UINT32', 4, 4),
    'ColorARGBInt': ('COLORARGBINT', 4, 4), 'Point2DInt': ('POINT2DINT', 4, 2), 'Rectangle2D': ('RECTANGLE2D', 8, 2),
    'float': ('FLOAT', 4, 4), 'Fraction': ('FRACTION', 4, 4), 'Angle': ('ANGLE', 4, 4), 'ColorARGB': ('COLORARGB', 16, 4),
    'ColorRGB': ('COLORRGB', 12, 4), 'Vector2D': ('VECTOR2D', 8, 4), 'Vector3D': ('VECTOR3D', 12, 4),
    'Euler2D': ('EULER2D', 8, 4), 'Euler3D': ('EULER3D', 12, 4), 'Plane2D': ('PLANE2D', 12, 4),
    'Plane3D': ('PLANE3D', 16, 4), 'Point2D': ('POINT2D', 8, 4), 'Point3D': ('POINT3D', 12, 4),
    'Quaternion': ('QUATERNION', 16, 4), 'Matrix': ('MATRIX', 36, 4),
}
OTHER_SIZE = {'TagDependency': 16, 'TagReflexive': 12, 'TagDataOffset': 20, 'TagString': 32, 'TagID': 4,
              'Pointer': 4, 'TagFourCC': 4, 'ScenarioScriptNodeValue': 4}

# Field kinds (tagedit.cpp)
K_VALUE, K_DEP, K_REFLEXIVE, K_DATA, K_STRING, K_ENUM, K_BITFIELD, K_RAW, K_TAGID = range(9)
# Flags
F_READ_ONLY, F_LISTED, F_UNREAD, F_ZERO_ON_SAVE, F_BOUNDS = 1, 2, 4, 8, 16
# Byte order of a field in a tag file
E_BIG, E_LITTLE = 0, 1

CLASSES = {}   # extension -> fourcc
FOURCC = [
    ('actor', 'actr'), ('actor_variant', 'actv'), ('antenna', 'ant!'), ('model_animations', 'antr'), ('biped', 'bipd'),
    ('bitmap', 'bitm'), ('spheroid', 'boom'), ('continuous_damage_effect', 'cdmg'), ('model_collision_geometry', 'coll'),
    ('color_table', 'colo'), ('contrail', 'cont'), ('device_control', 'ctrl'), ('decal', 'deca'),
    ('ui_widget_definition', 'DeLa'), ('input_device_defaults', 'devc'), ('device', 'devi'),
    ('detail_object_collection', 'dobc'), ('effect', 'effe'), ('equipment', 'eqip'), ('flag', 'flag'), ('fog', 'fog '),
    ('font', 'font'), ('material_effects', 'foot'), ('garbage', 'garb'), ('glow', 'glw!'), ('grenade_hud_interface', 'grhi'),
    ('hud_message_text', 'hmt '), ('hud_number', 'hud#'), ('hud_globals', 'hudg'), ('item', 'item'),
    ('item_collection', 'itmc'), ('damage_effect', 'jpt!'), ('lens_flare', 'lens'), ('lightning', 'elec'),
    ('device_light_fixture', 'lifi'), ('light', 'ligh'), ('sound_looping', 'lsnd'), ('device_machine', 'mach'),
    ('globals', 'matg'), ('meter', 'metr'), ('light_volume', 'mgs2'), ('gbxmodel', 'mod2'), ('model', 'mode'),
    ('multiplayer_scenario_description', 'mply'), ('preferences_network_game', 'ngpr'), ('object', 'obje'),
    ('particle', 'part'), ('particle_system', 'pctl'), ('physics', 'phys'), ('placeholder', 'plac'),
    ('point_physics', 'pphy'), ('projectile', 'proj'), ('weather_particle_system', 'rain'),
    ('scenario_structure_bsp', 'sbsp'), ('scenery', 'scen'), ('shader_transparent_chicago_extended', 'scex'),
    ('shader_transparent_chicago', 'schi'), ('scenario', 'scnr'), ('shader_environment', 'senv'),
    ('shader_transparent_glass', 'sgla'), ('shader', 'shdr'), ('sky', 'sky '), ('shader_transparent_meter', 'smet'),
    ('sound', 'snd!'), ('sound_environment', 'snde'), ('shader_model', 'soso'), ('shader_transparent_generic', 'sotr'),
    ('ui_widget_collection', 'Soul'), ('shader_transparent_plasma', 'spla'), ('sound_scenery', 'ssce'),
    ('string_list', 'str#'), ('shader_transparent_water', 'swat'), ('tag_collection', 'tagc'), ('camera_track', 'trak'),
    ('dialogue', 'udlg'), ('unit_hud_interface', 'unhi'), ('unit', 'unit'), ('unicode_string_list', 'ustr'),
    ('virtual_keyboard', 'vcky'), ('vehicle', 'vehi'), ('weapon', 'weap'), ('wind', 'wind'),
    ('weapon_hud_interface', 'wphi'),
]
for ext, cc in FOURCC: CLASSES[ext] = cc
VERSIONS = {'actor': 2, 'model_animations': 4, 'biped': 3, 'bitmap': 7, 'contrail': 3, 'effect': 4, 'equipment': 2,
            'item': 2, 'item_collection': 0, 'damage_effect': 6, 'lens_flare': 2, 'light': 3, 'sound_looping': 3,
            'gbxmodel': 5, 'globals': 3, 'model': 4, 'model_collision_geometry': 10, 'particle': 2,
            'particle_system': 4, 'physics': 4, 'placeholder': 2, 'preferences_network_game': 2, 'projectile': 5,
            'scenario_structure_bsp': 5, 'scenario': 2, 'shader_environment': 2, 'sound': 4, 'shader_model': 2,
            'shader_transparent_water': 2, 'camera_track': 2, 'unit': 2, 'virtual_keyboard': 2, 'weapon': 2,
            'weapon_hud_interface': 2}


def name_of(n, ignore_numbers):
    n = n.replace(' ', '_').replace("'", '').replace('(', '').replace(')', '')
    if not ignore_numbers and n[0].isnumeric(): n = '_' + n
    return n


def fourcc_int(cc):
    return (ord(cc[0]) << 24) | (ord(cc[1]) << 16) | (ord(cc[2]) << 8) | ord(cc[3])


def expand_classes(classes):
    c = list(classes)
    sup = [('object', ['unit', 'device', 'item', 'projectile', 'scenery', 'sound_scenery']), ('unit', ['vehicle', 'biped']),
           ('model', ['gbxmodel']), ('item', ['weapon', 'garbage', 'equipment']),
           ('shader', ['shader_environment', 'shader_model', 'shader_transparent_chicago',
                       'shader_transparent_chicago_extended', 'shader_transparent_glass', 'shader_transparent_meter',
                       'shader_transparent_plasma', 'shader_transparent_water', 'shader_transparent_generic']),
           ('device', ['device_control', 'device_light_fixture', 'device_machine'])]
    for s, subs in sup:
        if s in c: c += subs
    return [x for x in c if x not in ('object', 'unit', 'device', 'shader', 'item')]


def fields_of(s):
    return (fields_of(defs[s['inherits']]) if 'inherits' in s else []) + s['fields']


enums = [n for n, d in defs.items() if d['type'] == 'enum']
bitfields = [n for n, d in defs.items() if d['type'] == 'bitfield']
enum_index = {n: i for i, n in enumerate(enums)}
bit_index = {n: i for i, n in enumerate(bitfields)}

order, index = [], {}


def visit(name):
    if name in index: return index[name]
    index[name] = len(order)
    order.append(name)
    for f in fields_of(defs[name]):
        if f['type'] == 'TagReflexive': visit(f['struct'])
    return index[name]


roots = {}
for n, d in defs.items():
    if d['type'] == 'struct' and 'class' in d: roots[d['class']] = visit(n)
roots['ui_widget_collection'] = visit('TagCollection')   # (Invader reads these as tag collections)

out_fields, out_structs, out_classes, out_excluded = [], [], [], []
for sname in order:
    s = defs[sname]
    struct_read_only = s.get('read_only', False)
    first = len(out_fields)
    off = 0
    for f in fields_of(s):
        t = f['type']
        if t == 'pad':
            off += f['size']
            continue
        n = f.get('count', 1) * (2 if f.get('bounds') else 1)
        unread = bool(f.get('cache_only') or f.get('unused'))
        read_only = (struct_read_only or f.get('read_only', False)) and not (f.get('read_only') is False)
        listed = not f.get('hidden') and not unread
        flags = (F_READ_ONLY if read_only else 0) | (F_UNREAD if unread else 0) | \
                (F_ZERO_ON_SAVE if f.get('drop_on_extract_hidden') else 0) | (F_BOUNDS if f.get('bounds') else 0)
        endian = E_LITTLE if ('endian' in f and f['endian'] != 'big') else E_BIG
        vt, size, comp, sub, count = -1, 0, 0, -1, 0
        mask = 0
        classes, nclasses, excluded, nexcluded, rmin, rmax = -1, 0, -1, 0, 0, 0xFFFFFFFF
        if t in VALUE:
            kind = K_VALUE
            v, esize, comp = VALUE[t]
            vt, size = VT.index(v), esize * n
            count = 2 if f.get('bounds') else 1
        elif t == 'TagDependency':
            kind, size, vt = K_DEP, 16 * n, VT.index('DEPENDENCY')
            cl = expand_classes(f['classes'])
            if cl[0] == '*': classes, nclasses = -1, 0
            else:
                classes, nclasses = len(out_classes), len(cl)
                out_classes += [fourcc_int(CLASSES[c]) for c in cl]
        elif t == 'TagReflexive':
            kind, size, vt, sub = K_REFLEXIVE, 12 * n, VT.index('REFLEXIVE'), visit(f['struct'])
            rmin = f.get('minimum', 0)
            rmax = f['extended_maximum'] if 'extended_maximum' in f else f.get('maximum', 0xFFFFFFFF)
        elif t == 'TagDataOffset':
            kind, size, vt = K_DATA, 20 * n, VT.index('TAGDATAOFFSET')
        elif t == 'TagString':
            kind, size, vt = K_STRING, 32 * n, VT.index('TAGSTRING')
        elif t == 'TagID':
            kind, size, vt = K_TAGID, 4 * n, VT.index('TAGID')
        elif t in ('Pointer', 'TagFourCC'):
            if not unread and not f.get('hidden'): raise Exception('%s::%s: %s that Invader lists' % (sname, f['name'], t))
            kind, size, listed = K_RAW, OTHER_SIZE[t] * n, False
        elif t == 'ScenarioScriptNodeValue':
            kind, size, listed = K_RAW, 4 * n, False
        elif t in defs and defs[t]['type'] == 'enum':
            e = defs[t]
            kind, size, vt, sub, count = K_ENUM, 2 * n, VT.index('ENUM'), enum_index[t], 1
            ex = []
            for lst in (f.get('exclude', []), e.get('exclude', [])):
                for i in lst:
                    if 'struct' not in i or i['struct'] is None or sname in i['struct']:
                        k = e['options'].index(i['option'])
                        if k not in ex: ex.append(k)
            if ex: excluded, nexcluded = len(out_excluded), len(ex); out_excluded += ex
        elif t in defs and defs[t]['type'] == 'bitfield':
            b = defs[t]
            kind, size, vt, sub, count = K_BITFIELD, b['width'] // 8 * n, VT.index('BITMASK'), bit_index[t], 1
            mask = (1 << len(b['fields'])) - 1
            for c in b.get('cache_only', []): mask &= ~(1 << b['fields'].index(c))
            ex = 0
            for lst in (f.get('exclude', []), b.get('exclude', [])):
                for i in lst:
                    if 'struct' not in i or i['struct'] is None or sname in i['struct']:
                        ex |= 1 << b['fields'].index(i['field'])
            mask &= ~ex
            if mask == 0: listed = False
        elif t in defs and defs[t]['type'] == 'struct':
            raise Exception('%s::%s: a struct inside a struct' % (sname, f['name']))
        else:
            raise Exception('%s::%s: unknown type %s' % (sname, f['name'], t))
        if listed: flags |= F_LISTED
        member = name_of(f['name'], False).replace('-', '_')
        out_fields.append('{"%s", %d, %d, %d, %d, %d, %d, %d, %d, %du, %d, %d, %d, %d, %d, %du}' % (
            member, off, size, kind, vt, flags, endian, sub, count, mask, classes, nclasses, excluded, nexcluded,
            rmin, rmax & 0xFFFFFFFF))
        off += size
    if off != s['size']: raise Exception('%s: %d bytes, the definition says %d' % (sname, off, s['size']))
    out_structs.append('{"%s", %d, %d, %d}' % (sname, off, first, len(out_fields) - first))

out_enums, out_enum_names, out_bits, out_bit_names = [], [], [], []
for n in enums:
    opts = [name_of(o, True).lower() for o in defs[n]['options']]
    out_enums.append('{%d, %d}' % (len(out_enum_names), len(opts)))
    out_enum_names += ['"%s"' % o for o in opts]
for n in bitfields:
    b = defs[n]
    names = [name_of(x, False).lower() for x in b['fields']]
    out_bits.append('{%d, %d, %d}' % (len(out_bit_names), len(names), b['width']))
    out_bit_names += ['"%s"' % o for o in names]

out_roots = []
for ext, cc in FOURCC:
    out_roots.append('{"%s", 0x%08Xu, %d, %d}' % (ext, fourcc_int(cc), roots.get(ext, -1), VERSIONS.get(ext, 1)))

with open(sys.argv[2], 'w') as o:
    o.write('// Halo CE tag (HEK) definitions as Invader\'s tools read and write them (tagedit.cpp). Generated from Invader\'s\n')
    o.write('// tag definitions (src/tag/hek/definition/*.json) by gen_tag_defs.py; don\'t edit.\n')
    o.write('static const TdField kTdFields[] = {\n    ' + ',\n    '.join(out_fields) + '\n};\n')
    o.write('static const TdStruct kTdStructs[] = {\n    ' + ',\n    '.join(out_structs) + '\n};\n')
    o.write('static const unsigned kTdClasses[] = {' + ', '.join('0x%08Xu' % c for c in out_classes) + '};\n')
    o.write('static const unsigned short kTdExcluded[] = {' + ', '.join(str(x) for x in out_excluded) + '};\n')
    o.write('static const TdNames kTdEnums[] = {' + ', '.join(out_enums) + '};\n')
    o.write('static const char *const kTdEnumNames[] = {\n    ' + ',\n    '.join(out_enum_names) + '\n};\n')
    o.write('static const TdBits kTdBitfields[] = {' + ', '.join(out_bits) + '};\n')
    o.write('static const char *const kTdBitNames[] = {\n    ' + ',\n    '.join(out_bit_names) + '\n};\n')
    o.write('static const TdClass kTdTagClasses[] = {\n    ' + ',\n    '.join(out_roots) + '\n};\n')
print(len(order), 'structs,', len(out_fields), 'fields,', len(enums), 'enums,', len(bitfields), 'bitfields')
