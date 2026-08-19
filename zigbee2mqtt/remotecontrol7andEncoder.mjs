import {presets as e} from 'zigbee-herdsman-converters/lib/exposes';
import * as m from 'zigbee-herdsman-converters/lib/modernExtend';
import {Zcl} from 'zigbee-herdsman';

const OTA_CLUSTER_ID = 0xfc00;
const OTA_CONFIG_ATTR_ID = 0x0001;
const OTA_MANUFACTURER_CODE = 0x1234;
const OTA_COMMAND_MAX_LEN = 254;
const OTA_CODE_RE = /^[0-9A-Za-z]{3}$/;
const OTA_TOKEN_RE = /^[0-9A-Za-z_-]{16}$/;
const OTA_AUTH_CHALLENGE_RE = /^A\|[0-9a-fA-F]{16}\|[0-9a-fA-F]{64}$/;
const OTA_CLUSTER_NAME = 'jarzemOta';
const OTA_ATTR_NAME = 'otaCommand';

const ENDPOINTS = {
    switch1: 1,
    switch2: 2,
    switch3: 3,
    switch4: 4,
    switch5: 5,
    switch6: 6,
    enable_rs232: 7,
    enable_ota: 8,
    light: 9,
};

const COLOR_TEMP_MIN_MIRED = 154;
const COLOR_TEMP_MAX_MIRED = 333;

const endpointNameById = (endpointId) => {
    for (const [name, id] of Object.entries(ENDPOINTS)) {
        if (id === endpointId) {
            return name;
        }
    }
    return undefined;
};

const clampNumber = (value, min, max) => {
    const number = Number(value);
    if (!Number.isFinite(number)) {
        return min;
    }
    return Math.min(max, Math.max(min, Math.round(number)));
};

const lightEndpoint = (entity, meta) => meta?.device?.getEndpoint(ENDPOINTS.light) ?? entity;

const endpointNameFromStateKey = (key, entity, meta) => {
    if (typeof key === 'string' && key.startsWith('state_')) {
        return key.slice('state_'.length);
    }
    return meta?.endpoint_name ?? endpointNameById(entity.ID);
};

const endpointForName = (entity, meta, endpointName) => {
    const endpointId = ENDPOINTS[endpointName];
    return endpointId === undefined ? entity : meta?.device?.getEndpoint(endpointId) ?? entity;
};

const validateOtaCommand = (value) => {
    if (typeof value !== 'string') {
        throw new Error('OTA command must be a string');
    }
    if (value.length < 1 || value.length > OTA_COMMAND_MAX_LEN) {
        throw new Error(`OTA command length must be 1-${OTA_COMMAND_MAX_LEN} characters`);
    }

    if (value.startsWith('A|')) {
        if (!OTA_AUTH_CHALLENGE_RE.test(value)) {
            throw new Error('OTA auth challenge must be A|<16 hex message_id>|<64 hex challenge>');
        }
        return;
    }

    if (value.startsWith('C|')) {
        const token = value.slice(2);
        if (!OTA_TOKEN_RE.test(token)) {
            throw new Error('OTA check token must be exactly 16 base64url characters');
        }
        return;
    }

    const provisioning = value.startsWith('P|') ? value.slice(2) : value;
    const fields = provisioning.split('|');
    if (fields.length !== 5) {
        throw new Error('OTA command must be provisioning, C|TOKEN, or A|MESSAGE_ID|CHALLENGE');
    }
    if (!OTA_CODE_RE.test(fields[3])) {
        throw new Error('OTA firmware code must be exactly 3 alphanumeric characters');
    }
    if (!OTA_TOKEN_RE.test(fields[4])) {
        throw new Error('OTA token must be exactly 16 base64url characters');
    }
};

const remoteFromOnOff = {
    cluster: 'genOnOff',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        if (msg.data.onOff === undefined) {
            return;
        }
        const endpointName = endpointNameById(msg.endpoint.ID);
        if (!endpointName) {
            return;
        }
        return {[`state_${endpointName}`]: msg.data.onOff ? 'ON' : 'OFF'};
    },
};

const remoteFromBrightness = {
    cluster: 'genLevelCtrl',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        if (msg.endpoint.ID !== ENDPOINTS.light || msg.data.currentLevel === undefined) {
            return;
        }
        return {brightness_light: msg.data.currentLevel};
    },
};

const remoteFromColorTemperature = {
    cluster: 'lightingColorCtrl',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        if (msg.endpoint.ID !== ENDPOINTS.light) {
            return;
        }

        const result = {};
        if (msg.data.colorTemperature !== undefined) {
            result.color_temp_light = msg.data.colorTemperature;
        }
        if (msg.data.colorMode !== undefined) {
            result.color_mode_light = msg.data.colorMode === 2 ? 'color_temp' : msg.data.colorMode;
        }
        return Object.keys(result).length > 0 ? result : undefined;
    },
};

const remoteFromOtaCommand = {
    cluster: OTA_CLUSTER_NAME,
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        if (msg.meta?.manufacturerCode !== undefined && msg.meta.manufacturerCode !== OTA_MANUFACTURER_CODE) {
            return;
        }
        const value = msg.data?.[OTA_ATTR_NAME] ?? msg.data?.[OTA_CONFIG_ATTR_ID] ?? msg.data?.[String(OTA_CONFIG_ATTR_ID)];
        if (value === undefined || value === null) {
            return;
        }

        return {action: String(value)};
    },
};

const remoteToOnOff = {
    key: [
        'state',
        'state_switch1',
        'state_switch2',
        'state_switch3',
        'state_switch4',
        'state_switch5',
        'state_switch6',
        'state_enable_rs232',
        'state_enable_ota',
        'state_light',
    ],
    convertSet: async (entity, key, value, meta) => {
        const endpointName = endpointNameFromStateKey(key, entity, meta);
        const state = String(value).toUpperCase();
        const command = state === 'ON' ? 'on' : state === 'OFF' ? 'off' : 'toggle';
        await endpointForName(entity, meta, endpointName).command('genOnOff', command, {}, {disableDefaultResponse: false});
    },
    convertGet: async (entity) => {
        await entity.read('genOnOff', ['onOff']);
    },
};

const remoteToBrightness = {
    key: ['brightness', 'brightness_light'],
    convertSet: async (entity, key, value, meta) => {
        const brightness = clampNumber(value, 0, 254);
        await lightEndpoint(entity, meta).command(
            'genLevelCtrl',
            'moveToLevel',
            {level: brightness, transtime: 0},
            {disableDefaultResponse: false},
        );
    },
    convertGet: async (entity, key, meta) => {
        await lightEndpoint(entity, meta).read('genLevelCtrl', ['currentLevel']);
    },
};

const remoteToColorTemperature = {
    key: ['color_temp', 'color_temp_light'],
    convertSet: async (entity, key, value, meta) => {
        const colorTemp = clampNumber(value, COLOR_TEMP_MIN_MIRED, COLOR_TEMP_MAX_MIRED);
        await lightEndpoint(entity, meta).command(
            'lightingColorCtrl',
            'moveToColorTemp',
            {colortemp: colorTemp, transtime: 0},
            {disableDefaultResponse: false},
        );
    },
    convertGet: async (entity, key, meta) => {
        await lightEndpoint(entity, meta).read('lightingColorCtrl', ['colorTemperature', 'colorMode']);
    },
};

const remoteToOtaCommand = {
    key: ['ota_command'],
    convertSet: async (entity, key, value, meta) => {
        validateOtaCommand(value);
        const endpoint = meta.device.getEndpoint(ENDPOINTS.switch1) ?? entity;
        await endpoint.write(
            OTA_CLUSTER_NAME,
            {[OTA_ATTR_NAME]: value},
            {disableDefaultResponse: false, manufacturerCode: OTA_MANUFACTURER_CODE},
        );
        return {state: {}};
    },
};

const definition = {
    fingerprint: [
        {manufacturerName: 'Jaros', modelID: 'RemoteControl7Encoder'},
        {manufacturerName: 'JaroslavZ', modelID: 'ESP32-C6-ENC'},
    ],
    model: 'ESP32-C6-ENC',
    vendor: 'Jaros',
    description: 'RemoteControl7Encoder six outputs with brightness, white temperature, RS232 and OTA enable switches',
    extend: [
        m.deviceAddCustomCluster(OTA_CLUSTER_NAME, {
            name: OTA_CLUSTER_NAME,
            ID: OTA_CLUSTER_ID,
            manufacturerCode: OTA_MANUFACTURER_CODE,
            attributes: {
                [OTA_ATTR_NAME]: {
                    name: OTA_ATTR_NAME,
                    ID: OTA_CONFIG_ATTR_ID,
                    type: Zcl.DataType.CHAR_STR,
                    write: true,
                },
            },
            commands: {},
            commandsResponse: {},
        }),
    ],
    fromZigbee: [remoteFromOnOff, remoteFromBrightness, remoteFromColorTemperature, remoteFromOtaCommand],
    toZigbee: [remoteToOnOff, remoteToBrightness, remoteToColorTemperature, remoteToOtaCommand],
    exposes: [
        e.light().withBrightness().withColorTemp([COLOR_TEMP_MIN_MIRED, COLOR_TEMP_MAX_MIRED]).withEndpoint('light'),
        e.switch().withEndpoint('switch1'),
        e.switch().withEndpoint('switch2'),
        e.switch().withEndpoint('switch3'),
        e.switch().withEndpoint('switch4'),
        e.switch().withEndpoint('switch5'),
        e.switch().withEndpoint('switch6'),
        e.switch().withEndpoint('enable_rs232'),
        e.switch().withEndpoint('enable_ota'),
    ],
    endpoint: () => ENDPOINTS,
    options: [],
    meta: {multiEndpoint: true},
    configure: async (device, coordinatorEndpoint, logger) => {
        for (const endpointId of Object.values(ENDPOINTS)) {
            const endpoint = device.getEndpoint(endpointId);
            if (!endpoint) {
                logger?.warn?.(`RemoteControl7Encoder endpoint ${endpointId} not found during configure`);
                continue;
            }
            await endpoint.read('genOnOff', ['onOff']);
        }

        const lightEndpoint = device.getEndpoint(ENDPOINTS.light);
        if (lightEndpoint) {
            await lightEndpoint.read('genLevelCtrl', ['currentLevel']);
            await lightEndpoint.read('lightingColorCtrl', ['colorTemperature', 'colorMode']);
        }
    },
};

export default definition;
