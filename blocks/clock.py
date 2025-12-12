#!/usr/bin/env python3

# i3blocks config:
# [clock]
# command=./clock.py
# interval=60

from datetime import datetime

now = datetime.now()
print( f"\x1fT2\x1f \x1fB15\x1f\x1fF4\x1f\x1fT3\x1f 󰥔 \x1fT\x1f\x1fB\x1f\x1fF\x1f\x1fB4\x1f\x1fF1\x1f {now.strftime('%H:%M')} \x1fB\x1f\x1fF\x1f \x1fT\x1f", flush=True)
