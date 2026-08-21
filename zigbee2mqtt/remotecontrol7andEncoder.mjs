import {presets as e, access as ea} from 'zigbee-herdsman-converters/lib/exposes';
import * as m from 'zigbee-herdsman-converters/lib/modernExtend';
import {Zcl} from 'zigbee-herdsman';

const OTA_CLUSTER_ID=0xfc00, OTA_CONFIG_ATTR_ID=0x0001, OTA_MANUFACTURER_CODE=0x1234, OTA_ENDPOINT=10;
const OTA_ENABLE_CLUSTER_ID=0xfc01, OTA_STATUS_CLUSTER_ID=0xfc02, OTA_CONTROL_ENDPOINT=11;
const OTA_ENABLE_ATTR_ID=0x0000, OTA_STATUS_ATTR_ID=0x0000;
const OTA_ZIGBEE_WIRE_MAX=100;
const OTA_DIAG_LEN_RE=/^D\|LEN\|(100|[1-9][0-9]?)$/;
const OTA_CHALLENGE_RE=/^A\|[0-9A-Za-z_-]{11}\|[0-9A-Za-z_-]{86}$/;
const OTA_PROVISION_RE=/^P\|[0-9A-Za-z_-]+$/;
const OTA_CLUSTER_NAME='jarzemOta', OTA_ATTR_NAME='otaCommand';
const OTA_ENABLE_CLUSTER_NAME='jarzemOtaEnable', OTA_ENABLE_ATTR_NAME='enableOta';
const OTA_STATUS_CLUSTER_NAME='jarzemOtaStatus', OTA_STATUS_ATTR_NAME='otaStatus';
const OTA_CMD_TO_DEVICE='otaToDevice', OTA_CMD_FROM_DEVICE='otaFromDevice';
const OTA_CMD_TO_DEVICE_ID=0x04, OTA_CMD_FROM_DEVICE_ID=0x11;
const ENDPOINTS={button1:1,button2:2,button3:3,button4:4,button5:5,button6:6,button7:7,light:8,enable_rs232:9,ota_control:11};
const APPLICATION_ENDPOINTS=[1,2,3,4,5,6,7,8,9];
const COLOR_TEMP_MIN_MIRED=154,COLOR_TEMP_MAX_MIRED=333;
const OTA_STATUS_TEXT={
    0:'idle',
    1:'provisioning_started',
    2:'provisioning_complete',
    16:'firmware_update_started',
    17:'firmware_update_complete',
    32:'provisioning_error',
    33:'provisioning_timeout',
    48:'firmware_update_error',
    49:'firmware_verify_error',
    64:'firmware_skipped',
};
const endpointNameById=(id)=>Object.entries(ENDPOINTS).find(([,v])=>v===id)?.[0];
const clampNumber=(value,min,max)=>Math.min(max,Math.max(min,Math.round(Number(value)||min)));
const lightEndpoint=(entity,meta)=>meta?.device?.getEndpoint(ENDPOINTS.light)??entity;
const endpointNameFromStateKey=(key,entity,meta)=>typeof key==='string'&&key.startsWith('state_')?key.slice(6):(meta?.endpoint_name??endpointNameById(entity.ID));
const endpointForName=(entity,meta,name)=>ENDPOINTS[name]===undefined?entity:(meta?.device?.getEndpoint(ENDPOINTS[name])??entity);
const otaControlEndpoint=(entity,meta)=>meta?.device?.getEndpoint(OTA_CONTROL_ENDPOINT)??(entity?.ID===OTA_CONTROL_ENDPOINT?entity:undefined);
const b64urlDecode=(s)=>Buffer.from(s.replace(/-/g,'+').replace(/_/g,'/')+'='.repeat((4-s.length%4)%4),'base64');

const validateOtaCommand=(value)=>{
    if(typeof value!=='string')throw new Error('OTA command must be a string');
    const bytes=Buffer.byteLength(value,'utf8');
    if(bytes<1||bytes>OTA_ZIGBEE_WIRE_MAX)throw new Error(`OTA MQTT payload must be 1-${OTA_ZIGBEE_WIRE_MAX} bytes`);
    if(value==='D|PING'||value==='D|STOP'||OTA_DIAG_LEN_RE.test(value))return;
    if(value.startsWith('A|')){if(!OTA_CHALLENGE_RE.test(value))throw new Error('Challenge must be A|<11 base64url random>|<86 base64url ECDSA signature>');return;}
    if(value.startsWith('P|')){if(!OTA_PROVISION_RE.test(value))throw new Error('Provisioning must be P|<base64url AES-GCM ciphertext+tag>');return;}
    throw new Error('OTA command must be D|PING, D|LEN|N, D|STOP, A|RANDOM|SIGNATURE, or P|CIPHERTEXT');
};

const otaRadioValue=(value)=>{
    if(value.startsWith('A|')){const p=value.split('|');const random=b64urlDecode(p[1]),sig=b64urlDecode(p[2]);if(random.length!==8||sig.length!==64)throw new Error('Challenge binary length invalid');return Buffer.concat([random,sig]);}
    if(value.startsWith('P|'))return b64urlDecode(value.slice(2));
    return Buffer.from(value,'utf8');
};
const logOtaUplink=(msg,meta,payload)=>{const ieee=msg?.device?.ieeeAddr??meta?.device?.ieeeAddr??'unknown';const kind=payload.split('|',1)[0];meta?.logger?.info?.(`[OTA/ZIGBEE RX] kind=${kind} from=${ieee} endpoint=${msg?.endpoint?.ID??'?'} cluster=0x${OTA_CLUSTER_ID.toString(16)} bytes=${Buffer.byteLength(payload,'utf8')}`);};

const remoteFromOnOff={cluster:'genOnOff',type:['attributeReport','readResponse'],convert:(model,msg)=>{if(msg.data.onOff===undefined)return;const n=endpointNameById(msg.endpoint.ID);if(!n||n==='ota_control')return;return{[`state_${n}`]:msg.data.onOff?'ON':'OFF'};}};
const remoteFromBrightness={cluster:'genLevelCtrl',type:['attributeReport','readResponse'],convert:(model,msg)=>msg.endpoint.ID===ENDPOINTS.light&&msg.data.currentLevel!==undefined?{brightness_light:msg.data.currentLevel}:undefined};
const remoteFromColorTemperature={cluster:'lightingColorCtrl',type:['attributeReport','readResponse'],convert:(model,msg)=>{if(msg.endpoint.ID!==ENDPOINTS.light)return;const r={};if(msg.data.colorTemperature!==undefined)r.color_temp_light=msg.data.colorTemperature;if(msg.data.colorMode!==undefined)r.color_mode_light=msg.data.colorMode===2?'color_temp':msg.data.colorMode;return Object.keys(r).length?r:undefined;}};
const remoteFromOtaCommand={cluster:OTA_CLUSTER_NAME,type:['commandOtaFromDevice'],convert:(model,msg,publish,options,meta)=>{if(msg.endpoint.ID!==OTA_ENDPOINT)return;const v=msg.data?.payload;if(v==null)return;const payload=String(v);logOtaUplink(msg,meta,payload);return{action:payload};}};
const remoteFromOtaRaw={cluster:OTA_CLUSTER_NAME,type:['raw'],convert:(model,msg,publish,options,meta)=>{if(msg.endpoint.ID!==OTA_ENDPOINT)return;const raw=Buffer.isBuffer(msg.data)?msg.data:msg.data?.data;if(raw==null)return;const b=Buffer.isBuffer(raw)?raw:Buffer.from(raw);if(b.length<2)return;const direct=b.toString('utf8');if(/^(H|D|R)\|/.test(direct)){logOtaUplink(msg,meta,direct);return{action:direct};}if(b.length>=4&&b[2]===OTA_CMD_FROM_DEVICE_ID){const n=b[3];if(n<1||b.length<4+n)return;const payload=b.subarray(4,4+n).toString('utf8');logOtaUplink(msg,meta,payload);return{action:payload};}}};
const remoteFromOtaEnable={cluster:OTA_ENABLE_CLUSTER_NAME,type:['attributeReport','readResponse'],convert:(model,msg)=>{if(msg.endpoint.ID!==OTA_CONTROL_ENDPOINT||msg.data?.[OTA_ENABLE_ATTR_NAME]===undefined)return;return{enable_ota:msg.data[OTA_ENABLE_ATTR_NAME]?'ON':'OFF'};}};
const remoteFromOtaStatus={cluster:OTA_STATUS_CLUSTER_NAME,type:['attributeReport','readResponse'],convert:(model,msg)=>{if(msg.endpoint.ID!==OTA_CONTROL_ENDPOINT||msg.data?.[OTA_STATUS_ATTR_NAME]===undefined)return;const value=Number(msg.data[OTA_STATUS_ATTR_NAME]);return{ota_status:OTA_STATUS_TEXT[value]??`unknown_${value}`};}};

const remoteToOnOff={key:['state','state_button1','state_button2','state_button3','state_button4','state_button5','state_button6','state_button7','state_light','state_enable_rs232'],convertSet:async(entity,key,value,meta)=>{const n=endpointNameFromStateKey(key,entity,meta),state=String(value).toUpperCase(),cmd=state==='ON'?'on':state==='OFF'?'off':'toggle';await endpointForName(entity,meta,n).command('genOnOff',cmd,{}, {disableDefaultResponse:false});},convertGet:async(entity)=>{await entity.read('genOnOff',['onOff']);}};
const remoteToBrightness={key:['brightness','brightness_light'],convertSet:async(entity,key,value,meta)=>{await lightEndpoint(entity,meta).command('genLevelCtrl','moveToLevel',{level:clampNumber(value,0,254),transtime:0},{disableDefaultResponse:false});},convertGet:async(entity,key,meta)=>{await lightEndpoint(entity,meta).read('genLevelCtrl',['currentLevel']);}};
const remoteToColorTemperature={key:['color_temp','color_temp_light'],convertSet:async(entity,key,value,meta)=>{await lightEndpoint(entity,meta).command('lightingColorCtrl','moveToColorTemp',{colortemp:clampNumber(value,COLOR_TEMP_MIN_MIRED,COLOR_TEMP_MAX_MIRED),transtime:0},{disableDefaultResponse:false});},convertGet:async(entity,key,meta)=>{await lightEndpoint(entity,meta).read('lightingColorCtrl',['colorTemperature','colorMode']);}};
const remoteToOtaCommand={key:['ota_command'],convertSet:async(entity,key,value,meta)=>{validateOtaCommand(value);const endpoint=meta.device.getEndpoint(OTA_ENDPOINT);if(!endpoint)throw new Error(`OTA endpoint ${OTA_ENDPOINT} not found on device`);const kind=value.split('|',1)[0],radio=otaRadioValue(value);meta?.logger?.info?.(`[OTA/ZIGBEE TX] kind=${kind} -> ${meta?.device?.ieeeAddr??'unknown'} endpoint=${OTA_ENDPOINT} cluster=0x${OTA_CLUSTER_ID.toString(16)} attr=0x0001 mqtt_bytes=${Buffer.byteLength(value,'utf8')} radio_value_bytes=${radio.length}`);await endpoint.write(OTA_CLUSTER_NAME,{[OTA_ATTR_NAME]:radio},{manufacturerCode:OTA_MANUFACTURER_CODE});meta?.logger?.info?.(`[OTA/ZIGBEE TX] kind=${kind} write response OK endpoint=${OTA_ENDPOINT}`);return{state:{}};}};
const remoteToOtaEnable={key:['enable_ota'],convertSet:async(entity,key,value,meta)=>{const endpoint=otaControlEndpoint(entity,meta);if(!endpoint)throw new Error(`OTA control endpoint ${OTA_CONTROL_ENDPOINT} not found on device; re-interview device`);const enabled=String(value).toUpperCase()==='ON'||value===true||value===1;await endpoint.write(OTA_ENABLE_CLUSTER_NAME,{[OTA_ENABLE_ATTR_NAME]:enabled},{manufacturerCode:OTA_MANUFACTURER_CODE});return{state:{enable_ota:enabled?'ON':'OFF'}};},convertGet:async(entity,key,meta)=>{const endpoint=otaControlEndpoint(entity,meta);if(!endpoint)throw new Error(`OTA control endpoint ${OTA_CONTROL_ENDPOINT} not found on device; re-interview device`);await endpoint.read(OTA_ENABLE_CLUSTER_NAME,[OTA_ENABLE_ATTR_NAME],{manufacturerCode:OTA_MANUFACTURER_CODE});}};
const remoteGetOtaStatus={key:['ota_status'],convertGet:async(entity,key,meta)=>{const endpoint=otaControlEndpoint(entity,meta);if(!endpoint)throw new Error(`OTA control endpoint ${OTA_CONTROL_ENDPOINT} not found on device; re-interview device`);await endpoint.read(OTA_STATUS_CLUSTER_NAME,[OTA_STATUS_ATTR_NAME],{manufacturerCode:OTA_MANUFACTURER_CODE});}};

const definition={fingerprint:[{manufacturerName:'Jaros',modelID:'RemoteControl7Encoder'},{manufacturerName:'JaroslavZ',modelID:'ESP32-C6-ENC'}],model:'ESP32-C6-ENC',vendor:'Jaros',description:'RemoteControl7Encoder seven buttons, light brightness/white temperature, RS232, OTA transport endpoint 10 and OTA control/status endpoint 11',extend:[m.deviceAddCustomCluster(OTA_CLUSTER_NAME,{name:OTA_CLUSTER_NAME,ID:OTA_CLUSTER_ID,manufacturerCode:OTA_MANUFACTURER_CODE,attributes:{[OTA_ATTR_NAME]:{name:OTA_ATTR_NAME,ID:OTA_CONFIG_ATTR_ID,type:Zcl.DataType.OCTET_STR,write:true}},commands:{[OTA_CMD_TO_DEVICE]:{name:OTA_CMD_TO_DEVICE,ID:OTA_CMD_TO_DEVICE_ID,parameters:[{name:'payload',type:Zcl.DataType.CHAR_STR}]}},commandsResponse:{[OTA_CMD_FROM_DEVICE]:{name:OTA_CMD_FROM_DEVICE,ID:OTA_CMD_FROM_DEVICE_ID,parameters:[{name:'payload',type:Zcl.DataType.CHAR_STR}]}}}),m.deviceAddCustomCluster(OTA_ENABLE_CLUSTER_NAME,{name:OTA_ENABLE_CLUSTER_NAME,ID:OTA_ENABLE_CLUSTER_ID,manufacturerCode:OTA_MANUFACTURER_CODE,attributes:{[OTA_ENABLE_ATTR_NAME]:{name:OTA_ENABLE_ATTR_NAME,ID:OTA_ENABLE_ATTR_ID,type:Zcl.DataType.BOOLEAN,write:true}}}),m.deviceAddCustomCluster(OTA_STATUS_CLUSTER_NAME,{name:OTA_STATUS_CLUSTER_NAME,ID:OTA_STATUS_CLUSTER_ID,manufacturerCode:OTA_MANUFACTURER_CODE,attributes:{[OTA_STATUS_ATTR_NAME]:{name:OTA_STATUS_ATTR_NAME,ID:OTA_STATUS_ATTR_ID,type:Zcl.DataType.UINT8}}})],fromZigbee:[remoteFromOnOff,remoteFromBrightness,remoteFromColorTemperature,remoteFromOtaCommand,remoteFromOtaRaw,remoteFromOtaEnable,remoteFromOtaStatus],toZigbee:[remoteToOnOff,remoteToBrightness,remoteToColorTemperature,remoteToOtaCommand,remoteToOtaEnable,remoteGetOtaStatus],exposes:[e.switch().withEndpoint('button1'),e.switch().withEndpoint('button2'),e.switch().withEndpoint('button3'),e.switch().withEndpoint('button4'),e.switch().withEndpoint('button5'),e.switch().withEndpoint('button6'),e.switch().withEndpoint('button7'),e.light().withBrightness().withColorTemp([COLOR_TEMP_MIN_MIRED,COLOR_TEMP_MAX_MIRED]).withEndpoint('light'),e.switch().withEndpoint('enable_rs232'),e.binary('enable_ota',ea.STATE_SET,'ON','OFF').withEndpoint('ota_control'),e.enum('ota_status',ea.STATE_GET,Object.values(OTA_STATUS_TEXT)).withEndpoint('ota_control')],endpoint:()=>ENDPOINTS,options:[],meta:{multiEndpoint:true},configure:async(device,coordinatorEndpoint,logger)=>{for(const id of APPLICATION_ENDPOINTS){const ep=device.getEndpoint(id);if(!ep){logger?.warn?.(`RemoteControl7Encoder endpoint ${id} not found during configure`);continue;}await ep.read('genOnOff',['onOff']);}const lep=device.getEndpoint(ENDPOINTS.light);if(lep){await lep.read('genLevelCtrl',['currentLevel']);await lep.read('lightingColorCtrl',['colorTemperature','colorMode']);}if(!device.getEndpoint(OTA_ENDPOINT))logger?.warn?.(`RemoteControl7Encoder OTA endpoint ${OTA_ENDPOINT} not found`);const ctl=device.getEndpoint(OTA_CONTROL_ENDPOINT);if(!ctl){logger?.warn?.(`RemoteControl7Encoder OTA control endpoint ${OTA_CONTROL_ENDPOINT} not found; device interview must be refreshed`);}else{await ctl.read(OTA_ENABLE_CLUSTER_NAME,[OTA_ENABLE_ATTR_NAME],{manufacturerCode:OTA_MANUFACTURER_CODE});await ctl.read(OTA_STATUS_CLUSTER_NAME,[OTA_STATUS_ATTR_NAME],{manufacturerCode:OTA_MANUFACTURER_CODE});}}};
export default definition;
