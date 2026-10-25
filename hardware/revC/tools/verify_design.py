"""Verify exported nets, actual socket/drill coordinates and requested Rev C layout."""
import csv,hashlib,io,json,math,re,zipfile
from pathlib import Path
R=Path(__file__).resolve().parents[1]

def main():
    intent=json.loads((R/'design_intent.json').read_text(encoding='utf8'))
    expected={p['ref']:p for p in intent['parts']};errors=[];report={}
    for label in ['schematic','pcb']:
        raw=json.loads((R/'exports'/f'{label}_netlist.json').read_text(encoding='utf8'))
        actual={c['props']['Designator']:c for c in raw['components'].values()}
        if set(actual)!=set(expected):errors.append(f'{label}: component reference set differs')
        pins_checked=0;nets=set()
        for ref,part in expected.items():
            got=actual.get(ref,{});pins=got.get('pinInfoMap',{})
            if set(pins)!=set(part['pins']):errors.append(f'{label}: {ref} pin set differs')
            for pin,net in part['pins'].items():
                wanted='' if net is None else net;found=pins.get(pin,{}).get('net','')
                if found!=wanted:errors.append(f'{label}: {ref}.{pin}: {found!r} != {wanted!r}')
                if wanted:nets.add(wanted)
                pins_checked+=1
            if not ref.startswith('J') and got.get('props',{}).get('Manufacturer Part')!=part['mpn']:
                errors.append(f'{label}: {ref} manufacturer part differs')
        if any(x in actual for x in ['J1','U2','U3','U4']):errors.append(f'{label}: removed parts remain')
        if any('3V3' in n or '3.3V' in n for n in nets):errors.append(f'{label}: obsolete 3.3V net remains')
        report[label]={'components':len(actual),'pins_checked':pins_checked,'functional_nets':len(nets)}
    required={'Gerber_TopLayer.GTL','Gerber_BottomLayer.GBL','Gerber_TopSolderMaskLayer.GTS',
              'Gerber_BottomSolderMaskLayer.GBS','Gerber_TopSilkscreenLayer.GTO','Gerber_BoardOutlineLayer.GKO',
              'Drill_PTH_Through.DRL','Drill_NPTH_Through.DRL','Drill_PTH_Through_Via.DRL'}
    with zipfile.ZipFile(R/'exports/EasyCanad_RevC_Gerber.zip') as z:
        if z.testzip() or not required.issubset(z.namelist()):errors.append('Manufacturing ZIP is incomplete or corrupt')
        for name in required.intersection(z.namelist()):
            if not z.read(name).strip():errors.append('Empty manufacturing file: '+name)
        outline=z.read('Gerber_BoardOutlineLayer.GKO').decode('ascii')
        coord=[(int(x)/1e5,int(y)/1e5) for x,y in re.findall(r'X(-?\d+)Y(-?\d+)',outline)]
        dims=[max(p[i] for p in coord)-min(p[i] for p in coord) for i in [0,1]]
        if any(abs(a-b)>.001 for a,b in zip(dims,[45.72,40.64])):errors.append('Board dimensions differ')
        drills=[(float(x),float(y)) for x,y in re.findall(r'^X([\d.\-]+)Y([\d.\-]+)',z.read('Drill_PTH_Through.DRL').decode('ascii'),re.M)]
        npth=re.findall(r'^X([\d.\-]+)Y([\d.\-]+)',z.read('Drill_NPTH_Through.DRL').decode('ascii'),re.M)
        if len(npth)!=2:errors.append('Two mounting holes required')
        report['manufacturing']={'files':sorted(z.namelist()),'width_mm':dims[0],'height_mm':dims[1],
                                 'area_reduction_vs_revB_percent':round((1-dims[0]*dims[1]/(80.01*64.77))*100,2),'npth':npth}
    native=json.loads((R/'evidence/native_validation.json').read_text(encoding='utf8'))
    if not native.get('ok'):errors.append('Native EDA validation failed')
    v=native.get('value',{})
    if v.get('pcbDrc')!=[]:errors.append('PCB DRC is not empty')
    if any(d.get('rule')!='ATTRMATCHCODE' or d.get('type')!='warn' for d in v.get('schematicDrc',[])):
        errors.append('Unexpected schematic ERC findings')
    geometry=json.loads((R/'nano_socket_geometry.json').read_text(encoding='utf8'))
    pads={(p['ref'],p['pin']):p for p in v.get('socketPads',[])};max_error=max_drill_error=0
    if len(pads)!=23:errors.append('Nano socket must have 23 pads')
    else:
        origin=pads[('JN1','1')]
        for h in geometry['holes']:
            p=pads[(h['ref'],h['pin'])]
            error=math.hypot((p['x']-origin['x'])*.0254-h['x_mm'],(origin['y']-p['y'])*.0254-h['y_down_mm'])
            max_error=max(max_error,error)
            ex=origin['x']*.0254+h['x_mm'];ey=origin['y']*.0254-h['y_down_mm']
            de=min(math.hypot(x-ex,y-ey) for x,y in drills);max_drill_error=max(max_drill_error,de)
            if max(error,de)>.002:errors.append('Socket coordinate error: '+h['module_pin'])
    placed={p['ref']:p for p in v.get('placement',[])}
    grouped=all(r in placed for r in ['J2','J3','J4','J5','SW1','J7','JP1','JP2'])
    if grouped:
        grouped=(all(abs(placed[r]['x']-130)<.1 and placed[r]['rotation']==90 for r in ['J2','J3','J4','J5'])
                 and abs(placed['J3']['y']-placed['J4']['y']-300)<.1
                 and abs(placed['J4']['y']-placed['J5']['y']-300)<.1
                 and placed['J5']['y']-placed['J2']['y']>=380
                 and all(1250<=placed[r]['x']<=1770 and 650<=placed[r]['y']<=1200 for r in ['SW1','J7','JP1','JP2']))
    if not grouped:errors.append('PWM banks/control-zone placement differs')
    if len(v.get('mountingHoles',[]))!=2:errors.append('Native mounting hole count differs')
    rows=list(csv.DictReader(io.StringIO((R/'exports/EasyCanad_RevC_BOM.csv').read_text(encoding='utf-16')),delimiter='\t'))
    if sum(int(r['Quantity']) for r in rows)!=38:errors.append('Native BOM quantity differs')
    for n in ['系统电路示意图.svg','系统电路示意图.png','nano_socket_1to1.svg']:
        if not (R/'exports'/n).is_file():errors.append('Missing diagram: '+n)
    report.update(socket_mechanical={'pins':len(pads),'side_row_span_mm':geometry['side_row_span_mm'],
                  'tail_offset_mm':geometry['bottom_offset_from_D8_mm'],'native_max_error_mm':max_error,
                  'gerber_drill_max_error_mm':max_drill_error,'tolerance_mm':.002,
                  'source_step_crosscheck_max_error_mm':geometry['step_crosscheck_max_error_mm']},
                  grouped_interface_layout_passed=grouped,bom={'grouped_rows':len(rows),'placed_quantity':38},
                  native_eda={k:val for k,val in v.items() if k!='files'},
                  known_schematic_warning='ATTRMATCHCODE: 12 generic 2.54mm connectors; purchase by pin count, footprint, current rating and mating height.',
                  bench_test_status='Not performed. Hardware files only; TF logging firmware unchanged.')
    report['sha256']={p.relative_to(R).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((R/'exports').glob('*')) if p.is_file()}
    report['passed_file_checks']=not errors;report['errors']=errors
    (R/'evidence/validation_report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
    print(json.dumps({'passed':not errors,'errors':errors,'socket_mechanical':report['socket_mechanical'],'pcb':report['pcb']},ensure_ascii=True,indent=2))
    return 0 if not errors else 1
if __name__=='__main__':raise SystemExit(main())
