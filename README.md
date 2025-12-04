# pbar

A featherweight text-rendering wayland statusbar. `p` indicates that pbar works with anonymous pipe `|`. This bar is highly customizable and extensible.

## build

- pkg-config
- wayland
- fcft
- pixman

```sh
make
```

## usage

```
pbar is a featherweight text-rendering wayland statusbar.
pbar recieves utf-8 sequence from STDIN and sends pointer event actions to STDOUT.
sequence between a pair of '\x1f' will be escaped instead of being rendered directly.

        version         1.0
        usage           producer | pbar [options] | consumer

options are:
        -B color        set default background color (000000ff)
        -F color        set default foreground color (ffffffff)
        -T font         set default font (monospace)
        -o output       set wayland output
        -s seat         set wayland seat
        -b              place the bar at the bottom
        -g gap          set margin gap (0)
        -i interval     set pointer event throttle interval in ms (100)

escape sequence can be:
        Bcolor          set background color
        B               restore last background color
        Fcolor          set foreground color
        F               restore last foreground color
        Tfont           set font
        T               restore last font
        1action         set left button click action
        1               restore last left button click action
        2action         set middle button click action
        2               restore last middle button click action
        3action         set right button click action
        3               restore right button click action
        4action         set axis scroll down action
        4               restore last axis scroll down action
        5action         set axis scroll up action
        5               restore last axis scroll up action
        6action         set axis scroll left action
        6               restore last axis scroll left action
        7action         set axis scroll right action
        7               restore last axis scroll right action
        R               swap background color and foreground color
        D               delimiter between left/center and center/right part

color can be:
        rrggbb          without alpha
        rrggbbaa        with alpha

font can be: (see 'man fcft_from_name')
        name            font name
        name:k=v        with single attribute
        name:k=v:k=v    with multiple attributes

action can be:
        xxx             anything except for '\x1f'
        xxx {}          {} will be replaced with pointer x-coordinate
```

## example

- use ***i3blocks*** as the producer
- use ***niri msg*** as the consumer

```sh
i3blocks | sed --unbuffered -e '1d' -e 's/^.//' | jq --unbuffered -r 'reduce .[] as $item (""; . + $item.full_text)' | pbar | while read -r cmd; do niri msg action spawn-sh -- "$cmd"; done
```

