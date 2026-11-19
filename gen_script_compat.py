# Generates script_compat.inc from Invader's script definitions
# (invader/ext/riat/definitions/src/definition/definition.json):
#   python3 gen_script_compat.py definition.json script_compat.inc
import json, sys
d = json.load(open(sys.argv[1]))
bits = {'xbox': 1, 'gbx-custom': 2, 'mcc-cea': 4}
types = {'void': 'v', 'boolean': 'b', 'real': 'r', 'short': 's', 'long': 'l', 'object_list': 'o'}
out = ['// Script functions and globals that exist in some of Xbox / Custom Edition / MCC but not all.',
       "// Generated from Invader's script definitions (ext/riat/definitions/src/definition/definition.json).",
       '// engines: 1 = Xbox, 2 = Custom Edition, 4 = MCC (CEA). type: v void, b boolean, r real, s short, l long, o object list.',
       'struct ScriptName { const char *name; bool global; char type; unsigned char engines; };',
       'static const ScriptName kScriptNames[] = {']
for is_global, items in ((False, d['functions']), (True, d['globals'])):
    for e in items:
        mask = sum(b for k, b in bits.items() if k in e.get('engines', {}))
        if mask in (0, 7): continue
        out.append('    {"%s", %s, \'%s\', %d},' % (e['name'], 'true' if is_global else 'false', types[e['type']], mask))
out.append('};')
open(sys.argv[2], 'w').write('\n'.join(out) + '\n')

# script_types.inc: every Xbox function's return type (for gathering repeated script code into static scripts):
#   python3 gen_script_compat.py definition.json script_compat.inc script_types.inc
if len(sys.argv) > 3:
    out = ["// Every Xbox script function's return type (\"passthrough\": that of its arguments).",
           "// Generated from Invader's script definitions by gen_script_compat.py.",
           'struct ScriptType { const char *name; const char *type; };',
           'static const ScriptType kScriptTypes[] = {']
    for e in d['functions']:
        if 'xbox' in e.get('engines', {}):
            out.append('    {"%s", "%s"},' % (e['name'], e['type']))
    out.append('};')
    open(sys.argv[3], 'w').write('\n'.join(out) + '\n')
