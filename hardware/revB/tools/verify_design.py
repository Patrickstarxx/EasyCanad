"""Cross-check the delivered native netlists, explicit pin contract and Gerber package."""
import csv
import hashlib
import io
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    intent = json.loads((ROOT / 'design_intent.json').read_text(encoding='utf-8'))
    errors = []
    reports = {}
    for label, filename in [('schematic', 'schematic_netlist.json'), ('pcb', 'pcb_netlist.json')]:
        source = json.loads((ROOT / 'exports' / filename).read_text(encoding='utf-8'))
        actual = {c['props']['Designator']: c for c in source['components'].values()}
        expected = {p['ref']: p for p in intent['parts']}
        if set(actual) != set(expected):
            errors.append(f'{label}: component reference set differs')
        checked = 0
        nets = set()
        for ref, part in expected.items():
            if ref not in actual:
                continue
            pins = actual[ref]['pinInfoMap']
            if set(pins) != set(part['pins']):
                errors.append(f'{label}: {ref} pin-number set differs')
            for number, net in part['pins'].items():
                wanted = '' if net is None else net
                found = pins.get(number, {}).get('net', '')
                if found != wanted:
                    errors.append(f'{label}: {ref}.{number}: {found!r} != {wanted!r}')
                if wanted:
                    nets.add(wanted)
                checked += 1
            if not ref.startswith('J') and actual[ref]['props'].get('Manufacturer Part') != part['mpn']:
                errors.append(f'{label}: {ref} manufacturer part differs')
        reports[label] = {'components': len(actual), 'pins_checked': checked, 'functional_nets': len(nets)}

    gerber = ROOT / 'exports' / 'EasyCanad_RevB_Gerber.zip'
    required = {'Gerber_TopLayer.GTL', 'Gerber_BottomLayer.GBL',
                'Gerber_TopSolderMaskLayer.GTS', 'Gerber_BottomSolderMaskLayer.GBS',
                'Gerber_TopSilkscreenLayer.GTO', 'Gerber_BoardOutlineLayer.GKO',
                'Drill_PTH_Through.DRL', 'Drill_NPTH_Through.DRL', 'Drill_PTH_Through_Via.DRL'}
    with zipfile.ZipFile(gerber) as z:
        if z.testzip():
            errors.append('Gerber ZIP CRC failure')
        if not required.issubset(z.namelist()):
            errors.append('Gerber package is missing a required layer/drill file')
        for name in required.intersection(z.namelist()):
            if not z.read(name).strip():
                errors.append(f'Empty production file: {name}')
        reports['manufacturing'] = {'files': sorted(z.namelist()), 'required_files_present': required.issubset(z.namelist())}

    # The native exporter produces UTF-16 tab-separated text with a .csv suffix.
    native_bom = (ROOT / 'exports' / 'EasyCanad_RevB_BOM.csv').read_text(encoding='utf-16')
    rows = list(csv.DictReader(io.StringIO(native_bom), delimiter='\t'))
    quantity = sum(int(r['Quantity']) for r in rows)
    if quantity != len(intent['parts']):
        errors.append(f'BOM quantity {quantity} differs from component count')
    reports['bom'] = {'grouped_rows': len(rows), 'placed_quantity': quantity}

    api = json.loads((ROOT / 'evidence' / 'native_validation.json').read_text(encoding='utf-8'))
    if not api.get('ok'):
        errors.append('Native EDA validation command failed')
    value = api.get('value', {})
    if value.get('pcbDrc') != []:
        errors.append('PCB DRC is not empty')
    unexpected = [d for d in value.get('schematicDrc', [])
                  if d.get('type') != 'warn' or d.get('rule') != 'ATTRMATCHCODE']
    if unexpected:
        errors.append('Unexpected schematic ERC findings')
    if value.get('components') != len(intent['parts']):
        errors.append('Native PCB component count differs')
    reports['native_eda'] = {k: v for k, v in value.items() if k != 'files'}
    reports['known_schematic_warning'] = 'ATTRMATCHCODE: generic 2.54mm connector display properties differ from source library supplier metadata; use BOM_assembly.csv procurement criteria.'
    reports['bench_test_status'] = 'Not performed; hardware files only. See README.md.'
    reports['sha256'] = {}
    for file in sorted((ROOT / 'exports').glob('*')):
        if file.is_file() and file.name != 'pcb_placement.png':
            reports['sha256'][file.relative_to(ROOT).as_posix()] = hashlib.sha256(file.read_bytes()).hexdigest()
    reports['passed_file_checks'] = not errors
    reports['errors'] = errors
    out = ROOT / 'evidence' / 'validation_report.json'
    out.write_text(json.dumps(reports, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'passed': not errors, 'reports': {k: reports[k] for k in ['schematic', 'pcb', 'bom']}, 'errors': errors}, ensure_ascii=True, indent=2))
    return 0 if not errors else 1


if __name__ == '__main__':
    raise SystemExit(main())
