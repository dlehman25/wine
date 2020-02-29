#!/bin/bash

# <mapZone other="UTC+13" territory="001" type="Etc/GMT-13"/>


wget https://raw.githubusercontent.com/unicode-org/cldr/master/common/supplemental/windowsZones.xml -O windowsZones.xml > /dev/null 2>&1
if [ $? -ne 0 ]
then
    echo "failed to download windowsZones.xml"
    exit
fi

WUTMP="/tmp/win_unix_map.tmp"
WU="/tmp/win_unix_map"
grep 'territory="001"' windowsZones.xml | sed -r 's/^\s*<mapZone other="([^"]+)" territory="001" type="([^"]+)".*>$/\1,\2/g' | sort > "$WUTMP"
if [ ! -s "$WUTMP" ]
then
    echo "failed to create win_unix_map"
    exit
fi

cp -p "$WUTMP" "${WU}.lastgood"
mv "$WUTMP" "$WU"

rm -rf windowsZones.xml

ls -l "$WU"

