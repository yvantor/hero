#!/bin/sh

echo 1 > /proc/sys/kernel/printk
mount -t debugfs none /sys/kernel/debug ||:

insmod /usr/lib/pulp_module.ko

# Debugs
# cat /proc/iomem
# cat /sys/kernel/debug/kernel_page_tables

./bringup pulp/hello_world.bin | tee -a run.log

rmmod pulp_module.ko

