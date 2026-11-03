# AirTags through the ESPHome Bluetooth proxy (research, 2026-11-03)

Status: research only, nothing implemented. Sources are community reverse engineering and project
pages, not an Apple specification.

## What an AirTag sends

Apple manufacturer data 0x004C, type 0x12 (Find My), a status byte and most of a P-224 public key;
the rest of the key sits in the random static BLE address. Separated from its owner the address and
key change once a day (about 04:00 local time); near the owner every 15 minutes.
([AirTag notes](https://github.com/adamcatley/adamcatley.github.io/blob/master/docs/AirTag.md),
[arXiv 2604.10138](https://arxiv.org/pdf/2604.10138))

## What the gateway can do

| Option | Needs | Per AirTag | Verdict |
|---|---|---|---|
| A Find My device is near (type 0x12, anonymous) | nothing on the gateway: the proxy forwards the advertisements; a template or [Bermuda](https://github.com/agittins/bermuda/wiki) in Home Assistant | no: neighbours' AirTags and iPhones trigger it too | possible, weak signal |
| Own AirTags recognised locally | the keys from an Apple device (macOS keychain "BeaconStore", [FindMy.py](https://github.com/malmeloo/FindMy.py)), addresses precomputed per day and matched in Home Assistant or on the gateway | only with a manual key export on a Mac; key extraction is reported broken or needs SIP off on macOS Sequoia/Tahoe ([gist](https://gist.github.com/henrik242/1da3a252ca66fb7d17bca5509a67937f), [FindMySyncPlus](https://github.com/manonstreet/FindMySyncPlus/wiki)) | fragile, own development, not worth it now |
| Location through Apple (iCloud) | Apple ID, 2FA, Anisette and the same keys; no proxy involved | yes, delayed reports | outside the gateway |

Home Assistant's [Private BLE Device](https://www.home-assistant.io/integrations/private_ble_device/)
needs an IRK (iPhone, Apple Watch), which an AirTag does not have; the core iCloud integration does
not list AirTags ([forum](https://community.home-assistant.io/t/icloud-integration-airtag-support/325102)).

## Recommendation

For "keys at home" use a BLE tag with a fixed address or an iBeacon (Bermuda handles both through the
proxy). An AirTag separated from its owner also starts beeping after a while (Apple's anti-stalking
behaviour, [Apple](https://support.apple.com/guide/personal-safety/ips139b15fd9/web)), so it is a poor
presence tag. Automatic adding of individual AirTags is not possible without the owner's keys.
