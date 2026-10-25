"""Read manufacturer Altium pads and independently check their STEP hole circles.

Usage: python extract_nano_geometry.py manufacturer.PcbDoc manufacturer.step
The PcbDoc container requires olefile. Only geometry/data is read, never executed.
Altium pad-record field positions follow the documented KiCad importer.
"""
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import olefile

ROOT = Path(__file__).resolve().parents[1]
pcb, step = map(Path, sys.argv[1:3])
with olefile.OleFileIO(pcb) as doc:
    data = doc.openstream('Pads6/Data').read()
pos = 0
pads = []
while pos < len(data):
    assert data[pos] == 2
    pos += 1
    sub = []
    for _ in range(6):
        size = struct.unpack_from('<I', data, pos)[0]
        pos += 4
        sub.append(data[pos:pos + size])
        pos += size
    prop = sub[4]
    name = sub[0][1:1 + sub[0][0]].decode('ascii')
    comp = struct.unpack_from('<H', prop, 7)[0]
    x, y = struct.unpack_from('<ii', prop, 13)
    hole = struct.unpack_from('<I', prop, 45)[0]
    pads.append(dict(component=comp, name=name, x=x * .00000254,
                     y=y * .00000254, hole=hole * .00000254))
assert pos == len(data)
through = [p for p in pads if p['component'] < 3 and p['hole'] > .8]
# Each module edge has both castellations and interior through holes.
left = sorted([p for p in through if p['component'] == 0 and p['y'] > 74], key=lambda p:p['x'])
right = sorted([p for p in through if p['component'] == 1 and p['y'] < 90], key=lambda p:p['x'])
bottom = sorted([p for p in through if p['component'] == 2 and p['x'] < 95], key=lambda p:p['y'])
assert [len(left), len(right), len(bottom)] == [9, 9, 5]
origin = left[0]
rows = [('JN1', left, [f'D{i}' for i in range(9)]),
        ('JN2', right, ['5V','GND','VIN','A5','A4','A3','A2','A1','A0']),
        ('JN3', bottom, [f'D{i}' for i in range(9,14)])]
holes = []
for ref, row, labels in rows:
    for number, (p, label) in enumerate(zip(row, labels), 1):
        holes.append(dict(ref=ref, pin=str(number), module_pin=label,
                          x_mm=round(p['y']-origin['y'],8),
                          y_down_mm=round(p['x']-origin['x'],8),
                          source_component=p['component'], source_pad=p['name'],
                          source_x_mm=p['x'], source_y_mm=p['y'], module_drill_mm=p['hole']))

s = step.read_text(encoding='ascii',errors='replace')
entities = {int(m[1]):m[2] for m in re.finditer(r'#(\d+)\s*=\s*(.*?);',s,re.S)}
points = {}
for k,v in entities.items():
    if v.startswith('CARTESIAN_POINT'):
        xyz = re.search(r'\(\s*([^()]*)\)\s*\)\s*$',v,re.S)[1]
        points[k] = [float(x) for x in xyz.split(',')]
axes = {k:[int(n) for n in re.findall(r'#(\d+)',v)] for k,v in entities.items() if v.startswith('AXIS2_PLACEMENT_3D')}
circles = []
for k,v in entities.items():
    if v.startswith('CIRCLE'):
        radius=float(re.search(r',\s*([^,()]*)\)\s*$',v)[1])
        axis=int(re.findall(r'#(\d+)',v)[0])
        p=points[axes[axis][0]]
        if abs(radius-.4572)<.00001 and abs(p[2])<.00001:
            circles.append(p)
# STEP board axes: X points away from USB; +/-Y are the long edges.
step_origin=[1.590002540005,-7.73001524003]
max_error=0
for hole in holes:
    error=min(((p[1]-step_origin[1]-hole['x_mm'])**2+
               (p[0]-step_origin[0]-hole['y_down_mm'])**2)**.5 for p in circles)
    assert error < .001, (hole,error)
    max_error=max(max_error,error)

result=dict(source_forum='https://chat.nologo.tech/d/115',
            manufacturer_directory='https://ziliao.nologo.tech/无名科技Nologo资料/Arduino开发板/Supermini%20Nano',
            source_pcb=pcb.name, pcb_sha256=hashlib.sha256(pcb.read_bytes()).hexdigest(),
            source_step=step.name, step_sha256=hashlib.sha256(step.read_bytes()).hexdigest(),
            coordinate_definition='Carrier top view; USB up; D0 interior through-hole at (0,0), +X right, +Y down. Castellations excluded.',
            pitch_mm=2.54, side_row_span_mm=round(right[0]['y']-left[0]['y'],8),
            bottom_row_y_down_mm=holes[-1]['y_down_mm'],
            bottom_offset_from_D8_mm=round(holes[-1]['y_down_mm']-holes[8]['y_down_mm'],8),
            step_crosscheck_max_error_mm=max_error, holes=holes)
(ROOT/'nano_socket_geometry.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps({k:v for k,v in result.items() if k!='holes'},ensure_ascii=True,indent=2))
