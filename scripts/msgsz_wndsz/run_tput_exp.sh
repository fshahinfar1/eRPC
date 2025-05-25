#!/bin/bash
curdir=$(dirname $0)
app=$(cat $curdir/../autorun_app_file)
if [ "$app" != "msgsz_wndsz" ]; then
	echo "You need to configure and build small_rpc_tput app"
	echo "You can use:"
	echo "    $curdir/../select_app.sh small_rpc_tput"
	exit 1
fi

ssh_user=farbod
ssh_server=138.37.32.108
erpc_dir=/home/farbod/dev/eRPC
output_dir=$HOME/results/erpc

default_filename="/tmp/erpc_msgsz_wndsz_log.txt"
filename=$default_filename

TIME=30000
BATCH_SIZE=1
MSG_SIZE=64
NUMA_NODE=0
NUMA_NODE_SERVER=1
CONFIG_STR="NOT_GENERATED_YET"

parse_args() {
	while [ $# -gt 0 ]; do
		key=$1
		case $key in
			*)
				echo "Unexpected parameter: $key"
				exit 1
				;;
		esac
	done
}

stop_erverything() {
	cmd="sudo pkill msgsz_wndsz"
	ssh $ssh_user@$ssh_server "$cmd" &> /dev/null
	$cmd
	sleep 1
}

start_server() {
	cmd="bash $erpc_dir/scripts/msgsz_wndsz/run_app.sh -p 0 -n $NUMA_NODE_SERVER -m $MSG_SIZE -b $BATCH_SIZE -t $TIME &> $default_filename < /dev/null &"
	ssh $ssh_user@$ssh_server "$cmd" &> /dev/null
	sleep 1
}

start_client() {
	bash $curdir/run_app.sh -p 1 -n $NUMA_NODE \
		-m $MSG_SIZE -b $BATCH_SIZE -t $TIME &> "$filename"
	sleep 1
}

one_round() {
	stop_erverything
	start_server
	start_client
}

get_output_file_name() {
	echo "$output_dir"/msg_sz_"$1"_wnd_sz_"$2".txt
}

on_signal() {
	stop_erverything
	exit 1
}

trap 'on_signal' SIGINT SIGHUP

main() {
	parse_args $@
	if [ ! -d "$output_dir" ]; then mkdir -p "$output_dir"; fi

	# Fixed window size:
	msg_size=( 32 64 128 256 512 1024 2048 4096 8192 )
	wnd_size=( 512 )
	for wnd in "${wnd_size[@]}"; do
		for msg_sz in "${msg_size[@]}"; do
			echo "* Window size: $wnd Message size: $msg_sz"
			filename=$(get_output_file_name "$msg_sz" "$wnd")
			MSG_SIZE=$msg_sz
			BATCH_SIZE=$wnd
			one_round
		done
	done

	# Fixed message size:
	msg_size=( 8192 )
	wnd_size=( 1 2 4 8 16 32 64 128 256 512 1024 )
	for wnd in "${wnd_size[@]}"; do
		for msg_sz in "${msg_size[@]}"; do
			echo "* Window size: $wnd Message size: $msg_sz"
			filename=$(get_output_file_name "$msg_sz" "$wnd")
			MSG_SIZE=$msg_sz
			BATCH_SIZE=$wnd
			one_round
		done
	done
}

main $@
