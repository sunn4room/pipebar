# pbar

A featherweight text-rendering wayland statusbar. `p` indicates that pbar works with anonymous pipe `|`. This bar is highly customizable and extensible.

## build

- pkg-config
- wayland
- fcft
- pixman
- tllist

```sh
make
```

## usage

```
pbar is a featherweight text-rendering wayland statusbar.
pbar renders utf-8 sequence from STDIN and prints pointer event actions to STDOUT.
sequence between a pair of '\x1f' will be escaped instead of being rendered directly.

        version         2.0
        usage           producer | pbar [options] | consumer

options are:
        -c color,color  set colors list (000000ff,ffffffff)
        -f font,font    set fonts list (monospace)
        -o output       set wayland output
        -s seat         set wayland seat
        -b              place the bar at the bottom
        -g gap          set margin gap (0)
        -i interval     set pointer event throttle interval in ms (100)
        -r rep_str      set the replace string for action ({})

color can be:
        rrggbb          without alpha
        rrggbbaa        with alpha

font can be: (see 'man fcft_from_name')
        name            font name
        name:k=v        with single attribute
        name:k=v:k=v    with multiple attributes

environment variable:
        PBAR_COLORS     set colors list
        PBAR_FONTS      set fonts list

escape sequence can be:
        Bindex          set background color index
        B               restore last background color index
        Findex          set foreground color index
        F               restore last foreground color index
        Tindex          set font index
        T               restore last font index
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

index can be:
        0               the first item in colors/fonts list
        1               the second item in colors/fonts list
        ...             ...

action can be:
        xxx             anything except for '\x1f'
        xxx rep_str     rep_str will be replaced with pointer x-coordinate
```

## example

- use ***i3blocks*** as the producer
- use ***niri msg*** as the consumer

```sh
i3blocks | sed --unbuffered -e '1d' -e 's/^.//' | jq --unbuffered -r 'reduce .[] as $item (""; . + $item.full_text)' | pbar | while read -r cmd; do niri msg action spawn-sh -- "$cmd"; done
```

