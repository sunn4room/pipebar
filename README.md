# pipebar

![](screenshot.png)

A featherweight text-rendering wayland statusbar. `pipe` indicates that pipebar works with anonymous pipe `|`. This bar is highly customizable and extensible.

## install

```sh
# for archlinux
paru -S pipebar-git
```

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
pipebar is a featherweight text-rendering wayland statusbar.
It renders utf-8 sequence from STDIN line by line.
It prints mouse pointer event actions to STDOUT.

        version         3.3
        usage           producer | pipebar [options] | consumer

Options are:
        -c color,...    set colors list (000000ff,ffffffff)
        -f font,...     set fonts list (monospace)
        -o output,...   set wayland outputs list
        -s seat,...     set wayland seats list
        -b              place the bar at the bottom
        -g gap          set margin gap (0)
        -i interval     set pointer event throttle interval in ms (100)

color can be: (support 0/1/2/3/4/6/8 hex numbers)
        <empty>         -> 00000000
        g               -> ggggggff
        ga              -> ggggggaa
        rgb             -> rrggbbff
        rgba            -> rrggbbaa
        rrggbb          -> rrggbbff
        rrggbbaa        -> rrggbbaa

font can be: (see 'man fcft_from_name' 'man fonts-conf')
        name            font name
        name:k=v        with single attribute
        name:k=v:k=v    with multiple attributes

output/seat can be: (see 'wayland-info')
        name            output/seat name

Sequence between a pair of '\x1f' will be escaped instead of being rendered directly.
Valid escape sequences are:
        Bindex          set background color index (initially 0)
        B               restore to last background color index
        Findex          set foreground color index (initially 1)
        F               restore to last foreground color index
        Tindex          set font index (initially 0)
        T               restore to last font index
        Ooutput         set wayland output (initially NULL)
        O               restore to last wayland output
        1action         set left button click action (initially NULL)
        1               restore to last left button click action
        2action         set middle button click action (initially NULL)
        2               restore to last middle button click action
        3action         set right button click action (initially NULL)
        3               restore to last right button click action
        4action         set axis scroll down action (initially NULL)
        4               restore to last axis scroll down action
        5action         set axis scroll up action (initially NULL)
        5               restore to last axis scroll up action
        6action         set axis scroll left action (initially NULL)
        6               restore to last axis scroll left action
        7action         set axis scroll right action (initially NULL)
        7               restore to last axis scroll right action
        R               swap background color and foreground color
        D               delimiter between left/center and center/right part

index can be:
        0               the first item in colors/fonts list
        1               the second item in colors/fonts list
        ...             ...

action can be:
        xxx             anything except for '\x1f'
```

## convention

Considering the extensibility of pipebar, I make the following conventions.

### color list

Inspired by base16 theme.

- bg1
- fg1
- bg2
- fg2
- bg3
- fg3
- bg4
- fg4
- red
- orange
- yellow
- green
- cyan
- blue
- purple
- brown
- ...

There are some theme definitions in the themes folder.

### font list

- biggest monospace
- biggest sans-serif
- smaller monospace
- smaller sans-serif
- ...

## example

- use ***i3blocks*** as the producer
- use ***niri msg*** as the consumer

```sh
i3blocks | sed --unbuffered -e '1d' -e 's/^.//' | jq --unbuffered -r 'reduce .[] as $item (""; . + $item.full_text)' | pipebar -f monospace:size=18,sans-serif:size=18,monospace:size=12,sans-serif:size=12 -c 282a36,ffffff,363447,f0f1f4,44475a,f8f8f2,6272a4,9ea8c7,ff5555,ffb86c,f1fa8c,50fa7b,8be9fd,80bfff,ff79c6,bd93f9 | while read -r cmd; do niri msg action spawn-sh -- "$cmd"; done
```

There are some useful scripts in the blocks folder.
