#!/bin/bash
PREFIX="/tmp/hud"
for i in /tmp/hud*
do
	P=${i#$PREFIX}
	if [ "$P" == '*' ]
	then
		echo "No PIDS"
	else
		echo "Toggle hud for PID $P"
		kill -10 $P
	fi
done
