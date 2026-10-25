// Run through eda_cli.py against the opened delivery project. No design changes.
const p=await eda.dmt_Project.getCurrentProjectInfo();
await eda.dmt_EditorControl.openDocument(p.data[0].schematic.page[0].uuid);
await new Promise(r=>setTimeout(r,1200));
const schematicDrc=await eda.sch_Drc.check(true,false,true);
await eda.dmt_EditorControl.openDocument(p.data[0].pcb.uuid);
await new Promise(r=>setTimeout(r,1500));
const pcbDrc=await eda.pcb_Drc.check(true,false,true);
await eda.pcb_Document.zoomToBoardOutline();
const pic=await eda.dmt_EditorControl.getCurrentRenderedAreaImage(p.data[0].pcb.uuid+'@'+p.uuid);
const b64=await new Promise(resolve=>{const r=new FileReader();r.onload=()=>resolve(String(r.result).split(',')[1]);r.readAsDataURL(pic);});
return {projectName:p.name,projectUuid:p.uuid,pages:p.data[0].schematic.page.map(x=>x.name),schematicDrc,pcbDrc,components:(await eda.pcb_PrimitiveComponent.getAll()).length,tracks:(await eda.pcb_PrimitiveLine.getAll()).length,vias:(await eda.pcb_PrimitiveVia.getAll()).length,files:[{name:'pcb_routed.png',b64}]};
