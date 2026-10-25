// Run through eda_cli.py against the opened delivery project. No design changes.
const p=await eda.dmt_Project.getCurrentProjectInfo();
await eda.dmt_EditorControl.openDocument(p.data[0].schematic.page[0].uuid);
await new Promise(r=>setTimeout(r,1200));
const schematicDrc=await eda.sch_Drc.check(true,false,true);
const tabId=await eda.dmt_EditorControl.openDocument(p.data[0].pcb.uuid);
await new Promise(r=>setTimeout(r,1500));
const pcbDrc=await eda.pcb_Drc.check(true,false,true);
const components=await eda.pcb_PrimitiveComponent.getAll();
const allPads=await eda.pcb_PrimitivePad.getAll();const padById=new Map(allPads.map(x=>[x.getState_PrimitiveId(),x]));
const placement=components.map(c=>({ref:c.getState_Designator(),x:c.getState_X(),y:c.getState_Y(),rotation:c.getState_Rotation(),layer:c.getState_Layer()}));
const socketPads=[];
for(const c of components.filter(c=>/^JN[123]$/.test(c.getState_Designator())))for(const pp of await eda.pcb_PrimitiveComponent.getAllPinsByPrimitiveId(c.getState_PrimitiveId())){const native=padById.get(pp.getState_PrimitiveId())||pp;socketPads.push({ref:c.getState_Designator(),pin:pp.getState_PadNumber(),x:native.getState_X(),y:native.getState_Y(),net:native.getState_Net()});}
const mountingHoles=allPads.filter(x=>/^MH[1-4]$/.test(x.getState_PadNumber())).map(x=>({name:x.getState_PadNumber(),x:x.getState_X(),y:x.getState_Y(),hole:x.getState_Hole()}));
return {projectName:p.name,projectUuid:p.uuid,pages:p.data[0].schematic.page.map(x=>x.name),schematicDrc,pcbDrc,components:components.length,placement,socketPads,mountingHoles,tracks:(await eda.pcb_PrimitiveLine.getAll()).length,vias:(await eda.pcb_PrimitiveVia.getAll()).length};
