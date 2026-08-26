#!/bin/sh
# HTTP API one-liners. Run from a machine on the node's Wi-Fi.
NODE=${NODE:-http://10.42.0.1}
AUTH=${AUTH:-admin:retimesh}

# status (public)
curl -s $NODE/api/status | python3 -m json.tool

# bulletin board
curl -s $NODE/api/board
curl -s -X POST $NODE/api/board -H 'Content-Type: application/json' \
     -d '{"author":"ops","text":"Node maintenance at 18:00"}'

# settings (admin)
curl -s -u $AUTH $NODE/api/settings | python3 -m json.tool
curl -s -u $AUTH -X POST $NODE/api/settings/radio -H 'Content-Type: application/json' \
     -d '{"sf":9,"tx_dbm":10}'                       # applied live
curl -s -u $AUTH -X POST $NODE/api/settings/transport -H 'Content-Type: application/json' \
     -d '{"enabled":true,"lora_mode":1,"wifi_mode":1}' # full/full; restarts
curl -s -u $AUTH -X POST $NODE/api/settings/wifi -H 'Content-Type: application/json' \
     -d '{"security":"wpa2","password":"changeme123"}'  # restarts
