#!/system/bin/sh
# WSA turns Android location back on at every boot; switch it off once boot has finished.
until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 2; done
sleep 10
cmd location set-location-enabled false
