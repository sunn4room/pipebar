#!/usr/bin/env python3

# i3blocks config
# [system]
# command=./system.py
# interval=persist

import psutil
import time

psutil.cpu_percent()
io = psutil.net_io_counters()
while True:
    time.sleep(1)
    new_io = psutil.net_io_counters()
    io_bytes = (new_io.bytes_recv - io.bytes_recv) + (new_io.bytes_sent - io.bytes_sent)
    if io_bytes < 1000:
        io_str = f" {io_bytes:3d}B/s"
    elif io_bytes < 1000000:
        io_str = f"{int(io_bytes/1000):3d}kB/s"
    else:
        io_str = f"{int(io_bytes/1000000):3d}mB/s"
    io_str = f"\x1fB12\x1f\x1fF4\x1f\x1fT3\x1f 󰖟 \x1fT\x1f\x1fB\x1f\x1fF\x1f\x1fB4\x1f\x1fF1\x1f {io_str} \x1fB\x1f\x1fF\x1f"
    io = new_io

    mem_usage = psutil.virtual_memory().percent
    mem_str = f"\x1fB14\x1f\x1fF4\x1f\x1fT3\x1f  \x1fT\x1f\x1fB\x1f\x1fF\x1f\x1fB4\x1f\x1fF1\x1f{mem_usage:3.0f}% \x1fB\x1f\x1fF\x1f"

    cpu_usage = psutil.cpu_percent()
    cpu_str = f"\x1fB9\x1f\x1fF4\x1f\x1fT3\x1f  \x1fT\x1f\x1fB\x1f\x1fF\x1f\x1fB4\x1f\x1fF1\x1f{cpu_usage:3.0f}% \x1fB\x1f\x1fF\x1f"

    print(f"\x1fT2\x1f {io_str} {mem_str} {cpu_str} \x1fT\x1f", flush=True)
