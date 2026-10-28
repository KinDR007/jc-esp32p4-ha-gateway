"""Builds firmware/gateway/components/sound/sound_presets.json from readable class names.

Each preset: global exclusions and a list of events. An event groups YAMNet classes and carries the
detection parameters (see docs/sound-detection.md, phase 3). Fails on any unknown class name.

    .venv/bin/python gen_presets.py
"""
import csv
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, 'firmware', 'gateway', 'components', 'sound', 'sound_presets.json')

# kind: "impulse" (one event per occurrence) or "continuous" (on while it lasts)
# threshold: score 0..1; windows: consecutive 0.48 s windows to start; hold_s: stays on after the
# last hit; cooldown_s: no new event of this type for that long after one ended
EVENTS = {
    'speech': dict(name='Speech', kind='continuous', threshold=0.6, windows=2, hold_s=5, cooldown_s=0,
                   classes=['Speech', 'Child speech, kid speaking', 'Conversation', 'Narration, monologue'],
                   suppress=['Television', 'Radio', 'Music']),
    'shout_scream': dict(name='Shout or scream', kind='impulse', threshold=0.5, windows=1, hold_s=3, cooldown_s=30,
                         classes=['Shout', 'Bellow', 'Yell', 'Children shouting', 'Screaming']),
    'laughter': dict(name='Laughter', kind='impulse', threshold=0.5, windows=2, hold_s=3, cooldown_s=60,
                     classes=['Laughter', 'Baby laughter', 'Giggle', 'Snicker', 'Belly laugh', 'Chuckle, chortle']),
    'baby_cry': dict(name='Baby crying', kind='continuous', threshold=0.5, windows=2, hold_s=10, cooldown_s=0,
                     classes=['Baby cry, infant cry', 'Crying, sobbing']),
    'cough_sneeze': dict(name='Cough or sneeze', kind='impulse', threshold=0.5, windows=1, hold_s=2, cooldown_s=30,
                         classes=['Cough', 'Sneeze', 'Throat clearing']),
    'snoring': dict(name='Snoring', kind='continuous', threshold=0.5, windows=3, hold_s=20, cooldown_s=0,
                    classes=['Snoring']),
    'dog_bark': dict(name='Dog barking', kind='continuous', threshold=0.5, windows=1, hold_s=10, cooldown_s=60,
                     classes=['Dog', 'Bark', 'Yip', 'Howl', 'Bow-wow', 'Growling', 'Whimper (dog)']),
    'cat': dict(name='Cat', kind='impulse', threshold=0.5, windows=1, hold_s=5, cooldown_s=60,
                classes=['Cat', 'Purr', 'Meow', 'Hiss', 'Caterwaul']),
    'bird': dict(name='Bird', kind='impulse', threshold=0.5, windows=2, hold_s=5, cooldown_s=120,
                 classes=['Bird', 'Bird vocalization, bird call, bird song', 'Chirp, tweet', 'Squawk', 'Pigeon, dove',
                          'Crow', 'Caw']),
    'doorbell': dict(name='Doorbell', kind='impulse', threshold=0.5, windows=1, hold_s=3, cooldown_s=30,
                     classes=['Doorbell', 'Ding-dong'], device_class='doorbell'),
    'knock': dict(name='Knock', kind='impulse', threshold=0.5, windows=1, hold_s=3, cooldown_s=20, classes=['Knock']),
    'door': dict(name='Door', kind='impulse', threshold=0.5, windows=1, hold_s=2, cooldown_s=10,
                 classes=['Door', 'Slam', 'Sliding door']),
    'glass_break': dict(name='Glass breaking', kind='impulse', threshold=0.5, windows=1, hold_s=3, cooldown_s=30,
                        classes=['Shatter', 'Breaking', 'Smash, crash']),
    'smoke_alarm': dict(name='Smoke or fire alarm', kind='continuous', threshold=0.4, windows=2, hold_s=10,
                        cooldown_s=0, classes=['Smoke detector, smoke alarm', 'Fire alarm']),
    'alarm_siren': dict(name='Alarm or siren', kind='continuous', threshold=0.5, windows=2, hold_s=10, cooldown_s=0,
                        classes=['Alarm', 'Siren', 'Civil defense siren', 'Buzzer', 'Alarm clock']),
    'phone_ring': dict(name='Phone ringing', kind='impulse', threshold=0.5, windows=2, hold_s=5, cooldown_s=30,
                       classes=['Telephone', 'Telephone bell ringing', 'Ringtone']),
    'water_running': dict(name='Water running', kind='continuous', threshold=0.5, windows=3, hold_s=10, cooldown_s=0,
                          classes=['Water tap, faucet', 'Sink (filling or washing)', 'Bathtub (filling or washing)']),
    'toilet_flush': dict(name='Toilet flush', kind='impulse', threshold=0.5, windows=2, hold_s=5, cooldown_s=60,
                         classes=['Toilet flush']),
    'vacuum': dict(name='Vacuum cleaner', kind='continuous', threshold=0.5, windows=3, hold_s=15, cooldown_s=0,
                   classes=['Vacuum cleaner']),
    'appliance': dict(name='Kitchen or bathroom appliance', kind='continuous', threshold=0.5, windows=2, hold_s=10,
                      cooldown_s=0, classes=['Microwave oven', 'Blender', 'Hair dryer']),
    'gunshot_explosion': dict(name='Gunshot or explosion', kind='impulse', threshold=0.5, windows=1, hold_s=3,
                              cooldown_s=30, classes=['Gunshot, gunfire', 'Machine gun', 'Explosion', 'Boom']),
    'music': dict(name='Music', kind='continuous', threshold=0.6, windows=4, hold_s=30, cooldown_s=0, classes=['Music']),
}

# Not events: acoustic environment, silence, music details (a "Music" event can still be made).
EXCLUDE_TOPS = ['Channel, environment and background']
EXCLUDE_CLASSES = ['Silence']

PRESETS = {
    'household': dict(name='Household', events=[
        'speech', 'shout_scream', 'laughter', 'baby_cry', 'cough_sneeze', 'snoring', 'dog_bark', 'cat', 'doorbell',
        'knock', 'door', 'glass_break', 'smoke_alarm', 'alarm_siren', 'phone_ring', 'water_running', 'toilet_flush',
        'vacuum', 'appliance']),
    'security': dict(name='Security', events=[
        'glass_break', 'smoke_alarm', 'alarm_siren', 'shout_scream', 'door', 'knock', 'dog_bark', 'gunshot_explosion']),
    'animals': dict(name='Animals', events=['dog_bark', 'cat', 'bird']),
    'minimal': dict(name='Doorbell and alarms', events=['doorbell', 'knock', 'smoke_alarm', 'alarm_siren', 'glass_break']),
}


def main():
    rows = list(csv.DictReader(open(os.path.join(HERE, 'data', 'yamnet_class_map.csv'))))
    index = {r['display_name']: int(r['index']) for r in rows}
    catalog = json.load(open(os.path.join(ROOT, 'firmware', 'gateway', 'components', 'sound', 'sound_classes.json')))

    def idx(names):
        missing = [n for n in names if n not in index]
        if missing:
            raise SystemExit(f'unknown classes: {missing}')
        return [index[n] for n in names]

    exclude = sorted({c[0] for c in catalog if c[2] in EXCLUDE_TOPS} | set(idx(EXCLUDE_CLASSES)))
    out = {}
    for key, preset in PRESETS.items():
        events = []
        for eid in preset['events']:
            e = dict(EVENTS[eid])
            e['id'] = eid
            e['classes'] = idx(e['classes'])
            e['suppress'] = idx(e.get('suppress', []))
            e['enabled'] = True
            events.append(e)
        out[key] = {'name': preset['name'], 'exclude': exclude, 'events': events}
    with open(OUT, 'w') as f:
        json.dump(out, f, separators=(',', ':'))
    print(f'{len(out)} presets, {len(EVENTS)} event types -> {os.path.relpath(OUT, ROOT)} ({os.path.getsize(OUT)} B)')


if __name__ == '__main__':
    main()
