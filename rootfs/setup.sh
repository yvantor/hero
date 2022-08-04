#!/bin/sh
export KO=/lib/modules/5.16.9/extra

echo "Exported $KO as KO"

export LIBOMPTARGET_DEBUG=1

echo "Exported LIBOMPTARGET_DEBUG=$LIBOMPTARGET_DEBUG as"

export LIBOMPTARGET_INFO=4

echo "Exported LIBOMPTARGET_INFO=$LIBOMPTARGET_INFO"
