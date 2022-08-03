#!/bin/sh
export LIB=/lib/modules/5.16.9/extra
echo 1 > /proc/sys/kernel/printk
mount -t debugfs none /sys/kernel/debug ||:

insmod $LIB/pulp.ko

LIBOMPTARGET_DEBUG=1 LIBOMPTARGET_INFO=-1 ./$1

rmmod $LIB/pulp.ko
