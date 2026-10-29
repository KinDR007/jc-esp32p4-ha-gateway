"""Starts zigpy (the ZHA stack) with zigpy-zboss on the coordinator role's TCP bridge.

Checks that the ESP ZBOSS NCP speaks what zigpy-zboss expects: start (forming a network when asked),
network info, permit join, shutdown. Run in its own venv, the gateway in the coordinator role and no
other host connected (a new TCP client replaces the old one). Result: docs/home-assistant.md.

  python3 -m venv .venv && .venv/bin/pip install zigpy-zboss==2.0.5
  .venv/bin/python tests/zha/zigpy_zboss_check.py <host> zigbee.db form|noform
"""
import asyncio, logging, sys, time
import zigpy.config as conf
from zigpy_zboss.zigbee.application import ControllerApplication

async def main(db):
    app = ControllerApplication(({
        conf.CONF_DEVICE: {conf.CONF_DEVICE_PATH: f'socket://{sys.argv[1]}:6638'},
        conf.CONF_DATABASE: db,
    }))
    t0 = time.time()
    await app.startup(auto_form=sys.argv[3] == "form")
    ni, nd = app.state.network_info, app.state.node_info
    print(f'started in {time.time()-t0:.1f} s')
    print(f'coordinator ieee {nd.ieee} nwk {nd.nwk} model {getattr(nd, "model", None)}')
    print(f'channel {ni.channel} pan {ni.pan_id} epid {ni.extended_pan_id} key_set {bool(ni.network_key)} children {len(ni.children)}')
    await app.permit(10)
    print('permit join 10 s: ok')
    await asyncio.sleep(2)
    await app.shutdown()
    print('shutdown ok')

logging.basicConfig(level=logging.WARNING)
asyncio.run(main(sys.argv[2]))
