# Generates map_layouts.inc from Invader's tag definitions (invader/src/tag/hek/definition/*.json):
#   python3 gen_map_layouts.py <definition folder> map_layouts.inc
# Where the pointers are in a built Xbox map's tag data (maptrim.cpp): for every tag group's struct and every struct it
# contains, its size and its fields that point somewhere (a reference's path, a block's elements, a data field's bytes,
# a pointer), inherited fields and fields of structs inside it included.
import glob, json, os, re, sys

types = {}
for p in sorted(glob.glob(os.path.join(sys.argv[1], '*.json'))):
    for x in json.load(open(p)):
        types[x['name']] = x
here = os.path.dirname(os.path.abspath(__file__))
FOURCC = dict(re.findall(r"\('(\w+)', '(.{4})'\)", open(os.path.join(here, 'gen_tag_defs.py')).read()))

PRIM = {'float': 4, 'Angle': 4, 'Fraction': 4, 'int8': 1, 'uint8': 1, 'int16': 2, 'uint16': 2, 'Index': 2, 'int32': 4,
        'uint32': 4, 'TagID': 4, 'TagFourCC': 4, 'ColorARGBInt': 4, 'Point2DInt': 4, 'Rectangle2D': 8, 'ColorARGB': 16,
        'ColorRGB': 12, 'Point2D': 8, 'Point3D': 12, 'Vector2D': 8, 'Vector3D': 12, 'Euler2D': 8, 'Euler3D': 12,
        'Quaternion': 16, 'Plane2D': 12, 'Plane3D': 16, 'Matrix': 36, 'TagString': 32, 'ScenarioScriptNodeValue': 4}
SPECIAL = {'TagDependency': (16, 0), 'TagReflexive': (12, 1), 'TagDataOffset': (20, 2), 'Pointer': (4, 3)}
# Fields Invader's definitions keep as numbers that on the Xbox are pointers into the tag data: a model part's
# triangle offset, triangle offset 2 and vertex offset.
XBOX_POINTERS = {'ModelGeometryPart': [76, 80, 100]}

memo = {}
def layout(name):   # (size, [(offset, kind, element struct)])
    if name in memo: return memo[name]
    s = types[name]
    out, off = [], 0
    if s.get('inherits'):
        off, base = layout(s['inherits'])
        out += base
    for f in s['fields']:
        t = f['type']
        for _ in range(f.get('count', 1) * (2 if f.get('bounds') else 1)):
            if t == 'pad': off += f['size']; break
            if t in SPECIAL:
                out.append((off, SPECIAL[t][1], f.get('struct')))
                off += SPECIAL[t][0]
            elif t in PRIM: off += PRIM[t]
            elif t in types:
                tt = types[t]
                if tt['type'] == 'enum': off += 2
                elif tt['type'] == 'bitfield': off += tt['width'] // 8
                else:
                    sz, sub = layout(t)
                    out += [(off + o, k, e) for o, k, e in sub]
                    off += sz
            else: sys.exit('unknown type %s in %s' % (t, name))
    if s.get('size', off) != off: sys.exit('%s: %d bytes laid out, the definition says %d' % (name, off, s['size']))
    out += [(o, 3, None) for o in XBOX_POINTERS.get(name, [])]
    memo[name] = (off, sorted(out))
    return memo[name]

groups = {x['class']: x['name'] for x in types.values() if x.get('type') == 'struct' and x.get('class') not in (None, 'None')}
groups['ui_widget_collection'] = groups['tag_collection']   # (the same struct under another fourcc)
order, index = [], {}
def visit(name):
    if name in index: return index[name]
    index[name] = len(order)
    order.append(name)
    for _, k, e in layout(name)[1]:
        if k == 1: visit(e)
    return index[name]
roots = sorted((FOURCC[c], visit(n)) for c, n in groups.items() if c in FOURCC)
header, bsp = visit('ScenarioStructureBSPCompiledHeader'), visit('ScenarioStructureBSP')
scen_size, scen = layout('Scenario')
syntax = [o for o, k, _ in scen if k == 2][1]           # script syntax data (after the editor's script data)
bsps = [o for o, k, e in scen if e == 'ScenarioBSP'][0]
if [f['name'] for f in types['Scenario']['fields'] if f['type'] == 'TagDataOffset'][1] != 'script syntax data':
    sys.exit('the scenario\'s second data field isn\'t its script syntax data')

def cc(c): return '0x%08Xu' % ((ord(c[0]) << 24) | (ord(c[1]) << 16) | (ord(c[2]) << 8) | ord(c[3]))
fields, structs = [], []
for n in order:
    size, fl = layout(n)
    structs.append('{%du, %du, %du}' % (size, len(fields), len(fl)))
    fields += ['{%d, %d, %d}' % (o, k, index[e] if k == 1 else -1) for o, k, e in fl]
def wrap(items):
    lines, line = [], '   '
    for it in items:
        if len(line) + len(it) + 2 > 118: lines.append(line); line = '   '
        line += ' ' + it + ','
    return lines + [line]
out = ['// Where the pointers are in a built map\'s tag data (maptrim.cpp). Generated from Invader\'s tag definitions',
       '// (src/tag/hek/definition/*.json) by gen_map_layouts.py; don\'t edit.',
       'static const MlField kMlFields[] = {'] + wrap(fields) + ['};', 'static const MlStruct kMlStructs[] = {'] + \
      wrap(structs) + ['};', 'static const MlGroup kMlGroups[] = {'] + wrap(['{%s, %d}' % (cc(c), i) for c, i in roots]) + \
      ['};', 'static const int kMlBspHeader = %d, kMlBsp = %d;' % (header, bsp),
       'static const unsigned kMlScenarioSyntax = %d, kMlScenarioBsps = %d;   // (the scenario\'s script syntax data, its BSPs)' % (syntax, bsps)]
open(sys.argv[2], 'w', newline='\n').write('\n'.join(out) + '\n')
print(len(order), 'structs,', len(fields), 'fields,', len(roots), 'groups')
