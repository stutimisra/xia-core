#!/bin/sh

kill $(ps aux | grep '/bin/xcache' | awk '{print $2}')
LD_LIBRARY_PATH=~/xia-core/api/lib ~/xia-core/bin/xcache -h host1 -v -l7&