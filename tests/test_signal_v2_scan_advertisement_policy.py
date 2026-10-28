#!/usr/bin/env python3
"""Policy: V2 discovery advertises its own UUID; V1/default must not change."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/"src/ble_elm_transport.cpp").read_text()
codec=(root/"include/signal_v2_codec.h").read_text()
route=(root/"include/signal_v2_can_route.h").read_text()
owner=(root/"src/signal_v2_can_owner.h").read_text()
assert 'advertising_->addServiceUUID(signal_v2::Service);' in source
assert 'advertising_->addServiceUUID(cfg::kBleServiceUuid);' in source
assert '#if defined(M5CAN_SIGNAL_V2_EXPERIMENTAL)' in source
assert source.index('advertising_->addServiceUUID(signal_v2::Service);') < source.index(
  'advertising_->addServiceUUID(cfg::kBleServiceUuid);')
assert 'f9c17d00-2a3e-4e18-89a2-d7c1b539e000' in codec
assert 'enum class CanIdWidth' in route
assert 'Standard11' in route and 'Extended29' in route
assert 'if(!extended && id>=0x7E8U && id<=0x7EFU)' in route
assert 'if(!pinned_.enabledForThisTrial()' in owner
assert 'stop("11BIT_OBD_RESPONSE",6)' in owner
print("V2 service-only discovery, V1 isolation and protocol-neutral 11/29bit route PASS")
