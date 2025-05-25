#!/bin/bash
rte_sdk="$HOME/dev/dpdk-21.11/dpdk-21.11.9/"
curdir=$(dirname $0)
rootdir=$(realpath "$curdir/..")
app_name=$1
if [ -z "$app_name" -o ! -d $rootdir/apps/$app_name ]; then
	echo Not a valid app! select from below
	for name in $(ls $rootdir/apps); do
		echo "* $name"
	done
	exit 1
fi

echo $app_name > $rootdir/scripts/autorun_app_file
RTE_SDK=$rte_sdk cmake . -DPERF=ON -DTRANSPORT=dpdk
make -j
make $app_name
