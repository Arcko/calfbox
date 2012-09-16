import sys
import struct

sys.path = [__file__[0 : __file__.rfind('/')]] + sys.path

import cbox

cbox.Document.dump()
#cbox.GetThings("/doc/uuid/blabla/bla", ['uuid'], []).uuid
scene_uuid = cbox.GetThings("/scene/get_uuid", ['uuid'], []).uuid
layer_uuid = cbox.GetThings("/scene/status", ['layer'], []).layer[1]
rt_uuid = cbox.GetThings("/rt/get_uuid", ['uuid'], []).uuid
assert cbox.GetThings(cbox.Document.uuid_cmd(scene_uuid, "/status"), ['uuid'], []).uuid == scene_uuid
assert cbox.GetThings(cbox.Document.uuid_cmd(layer_uuid, "/status"), ['uuid'], []).uuid == layer_uuid
assert cbox.GetThings(cbox.Document.uuid_cmd(rt_uuid, "/status"), ['uuid'], []).uuid == rt_uuid

layers = cbox.GetThings("/scene/status", ['%layer'], []).layer
assert(len(layers) == 1)
assert(layers[1] == layer_uuid)

instr_uuid = cbox.GetThings(cbox.Document.uuid_cmd(layer_uuid, "/status"), ['instrument_uuid'], []).instrument_uuid
iname = cbox.GetThings(cbox.Document.uuid_cmd(layer_uuid, "/status"), ['instrument_name'], []).instrument_name
assert cbox.GetThings("/scene/instr/%s/status" % iname, ['uuid'], []).uuid == instr_uuid
