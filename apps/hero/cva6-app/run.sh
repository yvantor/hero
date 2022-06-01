#!/bin/sh

echo 1 > /proc/sys/kernel/printk
mount -t debugfs none /sys/kernel/debug ||:

insmod pulp.ko

# Debugs
# cat /proc/iomem
# cat /sys/kernel/debug/kernel_page_tables

./bringup hello_pulp.bin | tee -a run.log

rmmod pulp.ko

