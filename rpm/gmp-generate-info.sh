#!/bin/bash
set -e
if [ "$MIC_RUN" != "" ]; then
	echo "gmp-generate-info.sh - returning FAIL to postpone oneshot to first boot"
	exit 1
fi

