import projectDefinition from './remotecontrol7andEncoder.project.mjs';
import * as ota from '../external/ota_server/zigbee2mqtt/jarzem_secure_ota.mjs';

const augment=(definition)=>({
    ...definition,
    extend:[...(definition.extend??[]),...ota.extend],
    fromZigbee:[...(definition.fromZigbee??[]),...ota.fromZigbee],
    toZigbee:[...(definition.toZigbee??[]),...ota.toZigbee],
    exposes:[...(definition.exposes??[]),...ota.exposes],
    endpoint:(device)=>({
        ...(typeof definition.endpoint==='function'?definition.endpoint(device):{}),
        ...ota.endpointMap,
    }),
    configure:async(device,coordinatorEndpoint,logger)=>{
        if(definition.configure)await definition.configure(device,coordinatorEndpoint,logger);
        await ota.configure(device,coordinatorEndpoint,logger);
    },
    meta:{...(definition.meta??{}),multiEndpoint:true},
});

export default Array.isArray(projectDefinition)?projectDefinition.map(augment):augment(projectDefinition);
