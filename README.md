# pipebar

Pipebar is a Wayland statusbar reading content from STDIN and sending event action to STDOUT.

## Build

```
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
pipebar version 0.1.1

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

