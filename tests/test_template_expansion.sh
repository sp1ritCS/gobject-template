#!/bin/bash

set -e

if [[ -z $GOT_PATH ]]; then
	echo "GOT_PATH was unset"
	exit 1
fi

"$GOT_PATH" -l | while IFS= read -r template; do
	mkdir -p "$template"
	pushd "$template"
	"$GOT_PATH" "$template" "GotObject"
	gcc -c $(pkg-config --cflags gobject-2.0) "gotobject.c"
	popd
done
