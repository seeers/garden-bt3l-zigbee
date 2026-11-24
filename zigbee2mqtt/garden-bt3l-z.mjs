import {Zcl} from 'zigbee-herdsman';
import {on_off} from 'zigbee-herdsman-converters/converters/toZigbee';
import {access, presets} from 'zigbee-herdsman-converters/lib/exposes';
import {battery, deviceAddCustomCluster, onOff} from 'zigbee-herdsman-converters/lib/modernExtend';

const maxSeconds = 21600;
const clusterName = 'gardenIrrigation';
const irrigationCluster = deviceAddCustomCluster(clusterName, {
    name: clusterName,
    ID: 0xfc90,
    attributes: {},
    commands: {
        startTimed: {name: 'startTimed', ID: 0x00, parameters: [{name: 'seconds', type: Zcl.DataType.UINT32}]},
    },
    commandsResponse: {},
});

async function waterFor(entity, seconds) {
    if (!Number.isInteger(seconds) || seconds < 1 || seconds > maxSeconds) {
        throw new Error(`Bewässerungsdauer muss eine ganze Zahl zwischen 1 und ${maxSeconds} Sekunden sein.`);
    }
    await entity.command(clusterName, 'startTimed', {seconds}, {disableDefaultResponse: false});
}

const timedSwitch = {
    ...on_off,
    async convertSet(entity, key, value, meta) {
        const {state, on_time: seconds, off_wait_time: cooldown} = meta.message;
        if (typeof state === 'string' && state.toLowerCase() === 'on' && seconds != null) {
            if (cooldown != null && cooldown !== 0) {
                throw new Error('Für dieses Ventil muss off_wait_time 0 sein oder fehlen.');
            }
            return waterFor(entity, seconds);
        }
        return on_off.convertSet(entity, key, value, meta);
    },
};

const waterAction = {
    key: ['water_for'],
    convertSet: (entity, key, value) => waterFor(entity, value),
};

const switchSupport = onOff({powerOnBehavior: false});

export default {
    zigbeeModel: ['GARDEN-BT3L-Z'],
    model: 'GARDEN-BT3L-Z',
    vendor: 'ALDI',
    description: 'Bewässerungscomputer mit eigener BT3L-Zigbee-Firmware',
    extend: [
        irrigationCluster,
        {...switchSupport, toZigbee: [timedSwitch]},
        battery(),
        {
            isModernExtend: true,
            exposes: [presets.numeric('water_for', access.SET)
                .withLabel('Bewässern für')
                .withUnit('s')
                .withValueMin(1)
                .withValueMax(maxSeconds)
                .withValueStep(1)
                .withDescription('Startet die Bewässerung für die angegebene Anzahl Sekunden.')],
            toZigbee: [waterAction],
        },
    ],
};
