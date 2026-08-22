import {presets as e} from 'zigbee-herdsman-converters/lib/exposes';

const ENDPOINTS={button1:1,button2:2,button3:3,button4:4,button5:5,button6:6,button7:7,light:8,enable_rs232:9};
const APPLICATION_ENDPOINTS=[1,2,3,4,5,6,7,8,9];
const COLOR_TEMP_MIN_MIRED=154,COLOR_TEMP_MAX_MIRED=333;

const endpointNameById=(id)=>Object.entries(ENDPOINTS).find(([,v])=>v===id)?.[0];
const clampNumber=(value,min,max)=>Math.min(max,Math.max(min,Math.round(Number(value)||min)));
const lightEndpoint=(entity,meta)=>meta?.device?.getEndpoint(ENDPOINTS.light)??entity;
const endpointNameFromStateKey=(key,entity,meta)=>typeof key==='string'&&key.startsWith('state_')?key.slice(6):(meta?.endpoint_name??endpointNameById(entity.ID));
const endpointForName=(entity,meta,name)=>ENDPOINTS[name]===undefined?entity:(meta?.device?.getEndpoint(ENDPOINTS[name])??entity);

const remoteFromOnOff={cluster:'genOnOff',type:['attributeReport','readResponse'],convert:(model,msg)=>{
    if(msg.data.onOff===undefined)return;
    const name=endpointNameById(msg.endpoint.ID);
    if(!name)return;
    return{[`state_${name}`]:msg.data.onOff?'ON':'OFF'};
}};

const remoteFromBrightness={cluster:'genLevelCtrl',type:['attributeReport','readResponse'],convert:(model,msg)=>
    msg.endpoint.ID===ENDPOINTS.light&&msg.data.currentLevel!==undefined?{brightness_light:msg.data.currentLevel}:undefined};

const remoteFromColorTemperature={cluster:'lightingColorCtrl',type:['attributeReport','readResponse'],convert:(model,msg)=>{
    if(msg.endpoint.ID!==ENDPOINTS.light)return;
    const result={};
    if(msg.data.colorTemperature!==undefined)result.color_temp_light=msg.data.colorTemperature;
    if(msg.data.colorMode!==undefined)result.color_mode_light=msg.data.colorMode===2?'color_temp':msg.data.colorMode;
    return Object.keys(result).length?result:undefined;
}};

const remoteToOnOff={key:['state','state_button1','state_button2','state_button3','state_button4','state_button5','state_button6','state_button7','state_light','state_enable_rs232'],convertSet:async(entity,key,value,meta)=>{
    const name=endpointNameFromStateKey(key,entity,meta);
    const endpoint=endpointForName(entity,meta,name);
    const state=String(value).toUpperCase();
    const cmd=state==='ON'?'on':state==='OFF'?'off':'toggle';
    await endpoint.command('genOnOff',cmd,{}, {disableDefaultResponse:false});
    if(state==='ON'||state==='OFF')return{state:{[`state_${name}`]:state}};
    return{state:{}};
},convertGet:async(entity,key,meta)=>{
    const name=endpointNameFromStateKey(key,entity,meta);
    await endpointForName(entity,meta,name).read('genOnOff',['onOff']);
}};

const remoteToBrightness={key:['brightness','brightness_light'],convertSet:async(entity,key,value,meta)=>{
    const level=clampNumber(value,0,254);
    await lightEndpoint(entity,meta).command('genLevelCtrl','moveToLevel',{level,transtime:0},{disableDefaultResponse:false});
    return{state:{brightness_light:level}};
},convertGet:async(entity,key,meta)=>{await lightEndpoint(entity,meta).read('genLevelCtrl',['currentLevel']);}};

const remoteToColorTemperature={key:['color_temp','color_temp_light'],convertSet:async(entity,key,value,meta)=>{
    const colorTemp=clampNumber(value,COLOR_TEMP_MIN_MIRED,COLOR_TEMP_MAX_MIRED);
    await lightEndpoint(entity,meta).command('lightingColorCtrl','moveToColorTemp',{colortemp:colorTemp,transtime:0},{disableDefaultResponse:false});
    return{state:{color_temp_light:colorTemp,color_mode_light:'color_temp'}};
},convertGet:async(entity,key,meta)=>{await lightEndpoint(entity,meta).read('lightingColorCtrl',['colorTemperature','colorMode']);}};

const definition={
    fingerprint:[{manufacturerName:'Jaros',modelID:'RemoteControl7Encoder'},{manufacturerName:'JaroslavZ',modelID:'ESP32-C6-ENC'}],
    model:'ESP32-C6-ENC',
    vendor:'Jaros',
    description:'RemoteControl7Encoder seven buttons, light brightness/white temperature and RS232 control',
    fromZigbee:[remoteFromOnOff,remoteFromBrightness,remoteFromColorTemperature],
    toZigbee:[remoteToOnOff,remoteToBrightness,remoteToColorTemperature],
    exposes:[
        e.switch().withEndpoint('button1'),e.switch().withEndpoint('button2'),e.switch().withEndpoint('button3'),
        e.switch().withEndpoint('button4'),e.switch().withEndpoint('button5'),e.switch().withEndpoint('button6'),
        e.switch().withEndpoint('button7'),
        e.light().withBrightness().withColorTemp([COLOR_TEMP_MIN_MIRED,COLOR_TEMP_MAX_MIRED]).withEndpoint('light'),
        e.switch().withEndpoint('enable_rs232'),
    ],
    endpoint:()=>ENDPOINTS,
    options:[],
    meta:{multiEndpoint:true},
    configure:async(device,coordinatorEndpoint,logger)=>{
        for(const id of APPLICATION_ENDPOINTS){
            const ep=device.getEndpoint(id);
            if(!ep){logger?.warn?.(`RemoteControl7Encoder endpoint ${id} not found during configure`);continue;}
            await ep.read('genOnOff',['onOff']);
        }
        const light=device.getEndpoint(ENDPOINTS.light);
        if(light){
            await light.read('genLevelCtrl',['currentLevel']);
            await light.read('lightingColorCtrl',['colorTemperature','colorMode']);
        }
    },
};

export default definition;
