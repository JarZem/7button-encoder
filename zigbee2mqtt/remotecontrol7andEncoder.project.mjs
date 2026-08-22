import {presets as e} from 'zigbee-herdsman-converters/lib/exposes';

const ENDPOINTS={button1:1,button2:2,button3:3,button4:4,button5:5,button6:6,button7:7,light:8,enable_rs232:9};
const APPLICATION_ENDPOINTS=[1,2,3,4,5,6,8,9];
const COLOR_TEMP_MIN_MIRED=154,COLOR_TEMP_MAX_MIRED=333;
const READBACK_DELAY_MS=120;
const STALE_REPORT_GUARD_MS=800;
const pendingOnOff=new Map();
const pendingBrightness=new Map();

const endpointNameById=(id)=>Object.entries(ENDPOINTS).find(([,v])=>v===id)?.[0];
const clampNumber=(value,min,max)=>Math.min(max,Math.max(min,Math.round(Number(value)||min)));
const lightEndpoint=(entity,meta)=>meta?.device?.getEndpoint(ENDPOINTS.light)??entity;
const endpointNameFromStateKey=(key,entity,meta)=>typeof key==='string'&&key.startsWith('state_')?key.slice(6):(meta?.endpoint_name??endpointNameById(entity.ID));
const endpointForName=(entity,meta,name)=>ENDPOINTS[name]===undefined?entity:(meta?.device?.getEndpoint(ENDPOINTS[name])??entity);
const delay=(ms)=>new Promise((resolve)=>setTimeout(resolve,ms));
const levelToPercent=(level)=>Math.floor((clampNumber(level,0,254)*100+127)/254);
const percentToLevel=(percent)=>Math.floor((clampNumber(percent,0,100)*254+50)/100);
const representableBrightness=(level)=>percentToLevel(levelToPercent(level));

const remoteFromOnOff={cluster:'genOnOff',type:['attributeReport','readResponse'],convert:(model,msg)=>{
    if(msg.data.onOff===undefined)return;
    const name=endpointNameById(msg.endpoint.ID);
    // Endpoint 7 is a physical/local master function only. Endpoint 8 OnOff mirrors
    // aggregate state internally, but HA must not expose or consume either as a switch.
    if(!name||name==='button7'||name==='light')return;
    const state=msg.data.onOff?'ON':'OFF';
    const key=`${msg.device?.ieeeAddr??'unknown'}:${msg.endpoint.ID}`;
    const pending=pendingOnOff.get(key);
    if(pending){
        const expired=Date.now()>pending.until;
        if(expired){
            pendingOnOff.delete(key);
        }else if(msg.type==='attributeReport'){
            if(state!==pending.expected)return;
        }else if(msg.type==='readResponse'){
            pendingOnOff.delete(key);
        }
    }
    return{[`state_${name}`]:state};
}};

const remoteFromBrightness={cluster:'genLevelCtrl',type:['attributeReport','readResponse'],convert:(model,msg)=>{
    if(msg.endpoint.ID!==ENDPOINTS.light||msg.data.currentLevel===undefined)return;
    const level=Number(msg.data.currentLevel);
    const key=`${msg.device?.ieeeAddr??'unknown'}:${msg.endpoint.ID}`;
    const pending=pendingBrightness.get(key);
    if(pending){
        const expired=Date.now()>pending.until;
        if(expired){
            pendingBrightness.delete(key);
        }else if(msg.type==='attributeReport'){
            if(level!==pending.expected)return;
        }else if(msg.type==='readResponse'){
            pendingBrightness.delete(key);
        }
    }
    return{brightness_light:level};
}};

const remoteFromColorTemperature={cluster:'lightingColorCtrl',type:['attributeReport','readResponse'],convert:(model,msg)=>{
    if(msg.endpoint.ID!==ENDPOINTS.light)return;
    const result={};
    if(msg.data.colorTemperature!==undefined)result.color_temp_light=msg.data.colorTemperature;
    if(msg.data.colorMode!==undefined)result.color_mode_light=msg.data.colorMode===2?'color_temp':msg.data.colorMode;
    return Object.keys(result).length?result:undefined;
}};

const remoteToOnOff={key:['state','state_button1','state_button2','state_button3','state_button4','state_button5','state_button6','state_enable_rs232'],convertSet:async(entity,key,value,meta)=>{
    const name=endpointNameFromStateKey(key,entity,meta);
    const endpoint=endpointForName(entity,meta,name);
    const state=String(value).toUpperCase();
    const cmd=state==='ON'?'on':state==='OFF'?'off':'toggle';
    if(state==='ON'||state==='OFF')pendingOnOff.set(`${meta?.device?.ieeeAddr??'unknown'}:${endpoint.ID}`,{expected:state,until:Date.now()+STALE_REPORT_GUARD_MS});
    await endpoint.command('genOnOff',cmd,{}, {disableDefaultResponse:false});
    await delay(READBACK_DELAY_MS);
    await endpoint.read('genOnOff',['onOff']);
    return{state:{}};
},convertGet:async(entity,key,meta)=>{
    const name=endpointNameFromStateKey(key,entity,meta);
    await endpointForName(entity,meta,name).read('genOnOff',['onOff']);
}};

const remoteToBrightness={key:['brightness','brightness_light'],convertSet:async(entity,key,value,meta)=>{
    const level=representableBrightness(value);
    const endpoint=lightEndpoint(entity,meta);
    pendingBrightness.set(`${meta?.device?.ieeeAddr??'unknown'}:${endpoint.ID}`,{expected:level,until:Date.now()+STALE_REPORT_GUARD_MS});
    await endpoint.command('genLevelCtrl','moveToLevel',{level,transtime:0},{disableDefaultResponse:false});
    await delay(READBACK_DELAY_MS);
    await endpoint.read('genLevelCtrl',['currentLevel']);
    return{state:{}};
},convertGet:async(entity,key,meta)=>{await lightEndpoint(entity,meta).read('genLevelCtrl',['currentLevel']);}};

const remoteToColorTemperature={key:['color_temp','color_temp_light'],convertSet:async(entity,key,value,meta)=>{
    const colorTemp=clampNumber(value,COLOR_TEMP_MIN_MIRED,COLOR_TEMP_MAX_MIRED);
    const endpoint=lightEndpoint(entity,meta);
    await endpoint.command('lightingColorCtrl','moveToColorTemp',{colortemp:colorTemp,transtime:0},{disableDefaultResponse:false});
    await delay(READBACK_DELAY_MS);
    await endpoint.read('lightingColorCtrl',['colorTemperature','colorMode']);
    return{state:{}};
},convertGet:async(entity,key,meta)=>{await lightEndpoint(entity,meta).read('lightingColorCtrl',['colorTemperature','colorMode']);}};

const definition={
    fingerprint:[{manufacturerName:'Jaros',modelID:'RemoteControl7Encoder'},{manufacturerName:'JaroslavZ',modelID:'ESP32-C6-ENC'}],
    model:'ESP32-C6-ENC',
    vendor:'Jaros',
    description:'RemoteControl7Encoder: six output switches; button 7 is local-only master off/restore; brightness/white temperature and RS232 control',
    fromZigbee:[remoteFromOnOff,remoteFromBrightness,remoteFromColorTemperature],
    toZigbee:[remoteToOnOff,remoteToBrightness,remoteToColorTemperature],
    exposes:[
        e.switch().withEndpoint('button1'),e.switch().withEndpoint('button2'),e.switch().withEndpoint('button3'),
        e.switch().withEndpoint('button4'),e.switch().withEndpoint('button5'),e.switch().withEndpoint('button6'),
        e.light().removeFeature('state').withBrightness().withColorTemp([COLOR_TEMP_MIN_MIRED,COLOR_TEMP_MAX_MIRED]).withEndpoint('light'),
        e.switch().withEndpoint('enable_rs232'),
    ],
    endpoint:()=>ENDPOINTS,
    options:[],
    meta:{multiEndpoint:true},
    configure:async(device,coordinatorEndpoint,logger)=>{
        for(const id of APPLICATION_ENDPOINTS){
            const ep=device.getEndpoint(id);
            if(!ep){logger?.warn?.(`RemoteControl7Encoder endpoint ${id} not found during configure`);continue;}
            if(id!==ENDPOINTS.light)await ep.read('genOnOff',['onOff']);
        }
        const light=device.getEndpoint(ENDPOINTS.light);
        if(light){
            await light.read('genLevelCtrl',['currentLevel']);
            await light.read('lightingColorCtrl',['colorTemperature','colorMode']);
        }
    },
};

export default definition;
