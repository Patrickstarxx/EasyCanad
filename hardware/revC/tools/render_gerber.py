"""Render actual production Gerbers, independent of the EDA editor viewport.
Dependencies: gerbonara, resvg-py.
"""
from pathlib import Path
from gerbonara.layers import LayerStack
from gerbonara.rs274x import GerberFile
from gerbonara.excellon import ExcellonFile
from zipfile import ZipFile
import resvg_py
R=Path(__file__).resolve().parents[1]
with ZipFile(R/'exports/EasyCanad_RevC_Gerber.zip') as z:
    layers={}
    names={('top','copper'):'Gerber_TopLayer.GTL',('bottom','copper'):'Gerber_BottomLayer.GBL',
           ('top','mask'):'Gerber_TopSolderMaskLayer.GTS',('bottom','mask'):'Gerber_BottomSolderMaskLayer.GBS',
           ('top','silk'):'Gerber_TopSilkscreenLayer.GTO',('bottom','silk'):'Gerber_BottomSilkscreenLayer.GBO',
           ('mechanical','outline'):'Gerber_BoardOutlineLayer.GKO'}
    for key,name in names.items():layers[key]=GerberFile.from_string(z.read(name).decode('ascii'),filename=name)
    drills=[ExcellonFile.from_string(z.read(name).decode('ascii'),plated=plated,filename=name)
            for name,plated in [('Drill_PTH_Through.DRL',True),('Drill_PTH_Through_Via.DRL',True),('Drill_NPTH_Through.DRL',False)]]
    stack=LayerStack(graphic_layers=layers,drill_layers=drills)
for side in ['top','bottom']:
    svg=str(stack.to_pretty_svg(side=side,margin=1))
    stem='pcb_routed' if side=='top' else 'pcb_bottom'
    (R/'exports'/f'{stem}.svg').write_text(svg,encoding='utf8',newline='\n')
    (R/'exports'/f'{stem}.png').write_bytes(resvg_py.svg_to_bytes(svg_string=svg,width=1800,dpi=96))
print('Rendered top and bottom directly from manufacturing Gerbers')
