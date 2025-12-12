#!/usr/bin/env python3

# i3blocks config:
# [brightness]
# command=./brightness.py
# interval=once
# signal=2

import subprocess
import time

action1 = [
    "brightnessctl s 20%; pkill -SIGRTMIN+2 i3blocks",
    "brightnessctl s 40%; pkill -SIGRTMIN+2 i3blocks",
    "brightnessctl s 60%; pkill -SIGRTMIN+2 i3blocks",
    "brightnessctl s 80%; pkill -SIGRTMIN+2 i3blocks",
    "brightnessctl s 100%; pkill -SIGRTMIN+2 i3blocks",
]
action3 = "pkill -SIGRTMIN+2 i3blocks"
action4 = "brightnessctl s 5%-; pkill -SIGRTMIN+2 i3blocks"
action5 = "brightnessctl s +5%; pkill -SIGRTMIN+2 i3blocks"

while True:
    try:
        output = subprocess.check_output(["brightnessctl", "i"], text=True).strip().split()
        bright = output[8][1:-1]
        bright = f"{bright:>4} "
        bright_str = ""
        for i in range(5):
            bright_str = bright_str + f"\x1f1{action1[i]}\x1f{bright[i:i+1]}\x1f1\x1f"
        print(f"\x1fT2\x1f \x1fB10\x1f\x1fF4\x1f\x1fT3\x1f\x1f3{action3}\x1f 󰃠 \x1f3\x1f\x1fT\x1f\x1fB\x1f\x1fF\x1f\x1fB4\x1f\x1fF1\x1f\x1f4{action4}\x1f\x1f5{action5}\x1f{bright_str}\x1f4\x1f\x1f5\x1f\x1fB\x1f\x1fF\x1f \x1fT\x1f")
        break
    except:
        time.sleep(1)
