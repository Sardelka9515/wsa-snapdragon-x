#!/system/bin/sh
# WSA turns Android location back on at every boot, and Google Play services asks
# for a location within seconds, which makes WSA show "wants to access your
# location". Switch it off as soon as the location service is up, then keep it
# off for the first 2 minutes in case it gets switched on again.
LOG=/data/local/tmp/disable-location.log
echo "$(date '+%T') start" > $LOG
until [ -n "$(cmd location is-location-enabled 2>/dev/null)" ]; do sleep 0.2; done
i=0
while [ $i -lt 600 ]; do
    if [ "$(cmd location is-location-enabled 2>/dev/null)" = true ]; then
        cmd location set-location-enabled false
        echo "$(date '+%T') switched off (uptime $(cut -d' ' -f1 /proc/uptime))" >> $LOG
    fi
    sleep 0.2
    i=$((i + 1))
done
echo "$(date '+%T') done" >> $LOG
