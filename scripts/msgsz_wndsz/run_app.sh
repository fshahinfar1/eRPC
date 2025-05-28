#!/bin/bash
curdir=$(dirname $0)

PROC_ID=-1
NUMA_NODE=0
MSG_SIZE=64
BATCH_SIZE=1
TIME=20000
THREADS=1

usage() {
	prog_name=$(basename $0)
	echo "Usage $prog_name: $prog_name
	* -n|--numa: numa node 
	* -p|--proc: processor number
	* -m|--msgsz: message size
	* -b|--batch: batch/concurrency size
	* -t|--time: experiment duration
	* -x|--thread: number of threads
	* -h|--help: show this message
	"
}

parse_args() {
	while [ $# -gt 0 ]; do
		key=$1
		case $key in
			-n|--numa)
				NUMA_NODE=$2
				shift; shift
				;;
			-p|--proc)
				PROC_ID=$2
				shift; shift
				;;
			-m|--msgsz)
				MSG_SIZE=$2
				shift; shift
				;;
			-b|--batch)
				BATCH_SIZE=$2
				shift; shift
				;;
			-t|--time)
				TIME=$2
				shift; shift
				;;
			-x|--thread)
				THREADS=$2
				shift; shift
				;;
			-h|--help)
				usage
				exit 0
				;;
			*)
				echo Unexpected argument: $key
				exit 1
				;;
		esac
	done

	if [ $PROC_ID -lt 0 -o $PROC_ID -gt 1 ]; then
		echo "Unexpected processor id (required argument): Either 0 (server) or 1 (client)"
		echo "     The app supports more but this script is running a simple case"
		exit 1
	fi

}

getconfig() {
	echo "--test_ms $TIME
--sm_verbose 0
--batch_size 1
--concurrency $BATCH_SIZE
--msg_size $MSG_SIZE
--num_processes 2
--num_threads $THREADS
--numa_node $NUMA_NODE
--numa_0_ports 0
--numa_1_ports 0
"
}

check_autorun_prog() {
	app=$(cat $curdir/../autorun_app_file)
	if [ "$app" != "msgsz_wndsz" ]; then
		echo "You need to configure and build small_rpc_tput app"
		echo "You can use:"
		echo "    $curdir/select_app.sh small_rpc_tput"
		exit 1
	fi
}

main() {
	check_autorun_prog
	parse_args $@
	getconfig > $curdir/../../apps/msgsz_wndsz/config
	echo New config:
	cat $curdir/../../apps/msgsz_wndsz/config
	# The do.sh script should be called from eRPC root
	cd $curdir/../../
	bash ./scripts/do.sh "$PROC_ID" "$NUMA_NODE"
	echo "Done"
}

main $@
