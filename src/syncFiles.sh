#!/bin/bash
set -e

# Sync executable
bn=$(basename $1)
rpiaddr="setr-mathis.duckdns.org"

rsync -az $1/*.ko "pi@$rpiaddr:/home/pi/projects/laboratoire4/"

