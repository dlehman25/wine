#!/bin/bash

# sha=$(echo -n "West Asia Standard Time" | sha1sum) && echo $(("0x${sha:37:3}" << 4))
function get_tzres()
{
    local tz="$1"
    local sha=$(echo -n "$tz" | sha1sum)
    local last=${sha:37:3}
    local val=$((0x$last << 4))
    echo "  $val \"$tz\""
    val=$((val+1))
    tz=${tz/Standard/Daylight}
    echo "  $val \"$tz\""
}

while read line
do
    wintz=${line%,*}
    get_tzres "$wintz"
done < win_unix_map
