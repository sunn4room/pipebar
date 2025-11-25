# pipebar

Pipebar is a Wayland statusbar reading content from STDIN and sending event action to STDOUT.

## Build

```sh
make
```

Ensure that you have the following dependencies:

* wayland
* wayland-protocols
* fcft
* pixman
* pkg-config

## Usage

```
Pipebar is a Wayland statusbar reading content from STDIN and sending event action to STDOUT.
pipebar version 0.1.2

usage: <producer> | pipebar [options] | <consumer>
options are:
        -B <aarrggbb>   set default background color (FF000000)
        -F <aarrggbb>   set default foreground color (FFFFFFFF)
        -T <font>       set default font (monospace)
        -o <output>     put the bar at the special output monitor
        -b              put the bar at the bottom
        -g <gap>        set margin gap (0)
        -t <ms>         set action throttle time in ms (500)

producer can produce control characters between a pair of \x1f.
control characters are:
        Baarrggbb       set background color to aarrggbb
        B               set background color to default
        Faarrggbb       set foreground color to aarrggbb
        F               set foreground color to default
        R               swap foreground and background
        Tname:k=v       set font to name:k=v
        T               set font to default
        1action         print action for consumer when click left button
        2action         print action for consumer when click middle button
        3action         print action for consumer when click right button
        4action         print action for consumer when scroll axis down
        5action         print action for consumer when scroll axis up
        6action         print action for consumer when scroll axis left
        7action         print action for consumer when scroll axis right
        n               close action 1/2/3/4/5/6/7 area
        M               mark the current control state
        U               restore current control state to the marked
        D               restore current control state to default
        I               the delimeter between left/center and center/right block
```

## Example

* use i3blocks as producer
* use niri spawn as consumer

i3blocks config

```
[left]
command=printf '\x1f1notify-send left\x1fleft\n'
interval=once

[sep1]
command=printf '\x1fI\x1f\n'
interval=once

[center]
command=printf '\x1f1notify-send center\x1fcenter\n'
interval=once

[sep2]
command=printf '\x1fI\x1f\n'
interval=once

[right]
command=printf '\x1f1notify-send right\x1fright\n'
interval=once
```

run pipebar

```sh
i3blocks | sed --unbuffered -e '1d' -e 's/^.//' | jq --unbuffered -r 'reduce .[] as $item (""; . + $item.full_text)' | pipebar | while read -r cmd; do niri msg action spawn-sh -- "$cmd"; done
```

