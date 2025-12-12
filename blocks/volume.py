#!/usr/bin/env python3

# i3blocks config
# [volume]
# command=./volume.py
# interval=once
# signal=1

import subprocess
import time

action1 = "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle; pkill -SIGRTMIN+1 i3blocks"
action11 = [
    "wpctl set-volume @DEFAULT_AUDIO_SINK@ 20%; pkill -SIGRTMIN+1 i3blocks",
    "wpctl set-volume @DEFAULT_AUDIO_SINK@ 40%; pkill -SIGRTMIN+1 i3blocks",
    "wpctl set-volume @DEFAULT_AUDIO_SINK@ 60%; pkill -SIGRTMIN+1 i3blocks",
    "wpctl set-volume @DEFAULT_AUDIO_SINK@ 80%; pkill -SIGRTMIN+1 i3blocks",
    "wpctl set-volume @DEFAULT_AUDIO_SINK@ 100%; pkill -SIGRTMIN+1 i3blocks",
]
action3 = "pkill -SIGRTMIN+1 i3blocks"
action4 = "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-; pkill -SIGRTMIN+1 i3blocks"
action5 = "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+; pkill -SIGRTMIN+1 i3blocks"

while True:
    try:
        output = subprocess.check_output(["wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"], text=True).strip().split(" ", 2)
        is_mute = len(output) == 3
        volume = int(float(output[1]) * 100)
        volume = f"{volume:3d}% "
        volume_str = ""
        for i in range(5):
            volume_str = volume_str + f"\x1f1{action11[i]}\x1f{volume[i:i+1]}\x1f1\x1f"
        print(f"\x1fT2\x1f \x1fB{"8" if is_mute else "11"}\x1f\x1fF4\x1f\x1fT3\x1f\x1f1{action1}\x1f\x1f3{action3}\x1f {"" if is_mute else ""} \x1f3\x1f\x1f1\x1f\x1fT\x1f\x1fB\x1f\x1fF\x1f\x1fB4\x1f\x1fF1\x1f\x1f4{action4}\x1f\x1f5{action5}\x1f{volume_str}\x1f4\x1f\x1f5\x1f\x1fB\x1f\x1fF\x1f \x1fT\x1f")
        break
    except:
        time.sleep(1)
