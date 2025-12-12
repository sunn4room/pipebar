#!/usr/bin/env python3

# i3blocks config
# [niri-windows]
# command=./niri-windows.py
# interval=persist

import json
import subprocess


def get_icon(key):
    icons = {
        1: "󰼏",
        2: "󰼐",
        3: "󰼑",
        4: "󰼒",
        5: "󰼓",
        6: "󰼔",
        7: "󰼕",
        8: "󰼖",
        9: "󰼗",
        "microsoft-edge": "",
        "firefox": "",
        "chromium": "",
        "Code": "",
        "mpv": "",
        "foot": "",
        "footclient": "",
        "Obsidian": "",
        "lf": "",
        "btop++": "",
        "VirtualBox Manager": "󰍺",
        "VirtualBox Machine": "󰍹",
    }
    if key not in icons:
        if type(key) is str:
            return ""
        else:
            return "󰼘"
    else:
        return icons[key]


event_stream = subprocess.Popen(
    ["niri", "msg", "--json", "event-stream"],
    stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
    universal_newlines=True,
    bufsize=1,
)

workspaces_raw = []
windows_raw = []

for line in event_stream.stdout:
    if line.startswith('{"Window') or line.startswith('{"Workspace'):
        update_raw = json.loads(line)
        for key, value in update_raw.items():
            match key:
                case "WorkspacesChanged":
                    workspaces_raw = value["workspaces"]
                case "WorkspaceActivated":
                    activated_workspace_output = ""
                    for workspace in workspaces_raw:
                        if workspace["id"] == value["id"]:
                            activated_workspace_output = workspace["output"]
                            break
                    for workspace in workspaces_raw:
                        if workspace["id"] == value["id"]:
                            workspace["is_active"] = True
                            workspace["is_focused"] = value["focused"]
                        else:
                            if activated_workspace_output == workspace["output"]:
                                workspace["is_active"] = False
                            if value["focused"]:
                                workspace["is_focused"] = False
                case "WorkspaceActiveWindowChanged":
                    for workspace in workspaces_raw:
                        if workspace["id"] == value["workspace_id"]:
                            workspace["active_window_id"] = value["active_window_id"]
                            break
                case "WorkspaceUrgencyChanged":
                    for workspace in workspaces_raw:
                        if workspace["id"] == value["id"]:
                            workspace["is_urgent"] = value["urgent"]
                            break
                case "WindowsChanged":
                    windows_raw = value["windows"]
                case "WindowOpenedOrChanged":
                    changed_window_idx = -1
                    for window_idx, window in enumerate(windows_raw):
                        if window["id"] == value["window"]["id"]:
                            changed_window_idx = window_idx
                            break
                    if changed_window_idx != -1:
                        del windows_raw[changed_window_idx]
                    if value["window"]["is_focused"]:
                        for window in windows_raw:
                            window["is_focused"] = False
                    windows_raw.append(value["window"])
                case "WindowUrgencyChanged":
                    for window in windows_raw:
                        if window["id"] == value["id"]:
                            window["is_urgent"] = value["urgent"]
                            break
                case "WindowFocusChanged":
                    for window in windows_raw:
                        if window["id"] == value["id"]:
                            window["is_focused"] = True
                        else:
                            window["is_focused"] = False
                case "WindowLayoutsChanged":
                    changes = {}
                    for change in value["changes"]:
                        changes[change[0]] = change[1]
                    for window in windows_raw:
                        if window["id"] in changes:
                            window["layout"] = changes[window["id"]]
                case "WindowClosed":
                    closed_window_idx = -1
                    for window_idx, window in enumerate(windows_raw):
                        if window["id"] == value["id"]:
                            closed_window_idx = window_idx
                            break
                    del windows_raw[closed_window_idx]

        workspaces = {}
        for workspace in workspaces_raw:
            workspaces[workspace["id"]] = workspace

        info = {}
        for workspace in workspaces_raw:
            if workspace["output"] not in info:
                info[workspace["output"]] = {}
            info[workspace["output"]][workspace["idx"]] = {}
            info[workspace["output"]][workspace["idx"]]["is_urgent"] = workspace["is_urgent"]
            info[workspace["output"]][workspace["idx"]]["is_active"] = workspace["is_active"]
            info[workspace["output"]][workspace["idx"]]["active_window_id"] = workspace["active_window_id"]
        for window in windows_raw:
            if window["is_floating"]:
                continue
            workspace = workspaces[window["workspace_id"]]
            if not workspace["is_active"]:
                continue
            if "windows" not in info[workspace["output"]][workspace["idx"]]:
                info[workspace["output"]][workspace["idx"]]["windows"] = {}
            if window["layout"]["pos_in_scrolling_layout"][0] not in info[workspace["output"]][workspace["idx"]]["windows"]:
                info[workspace["output"]][workspace["idx"]]["windows"][window["layout"]["pos_in_scrolling_layout"][0]] = {}
            info[workspace["output"]][workspace["idx"]]["windows"][window["layout"]["pos_in_scrolling_layout"][0]][window["layout"]["pos_in_scrolling_layout"][1]] = {}
            info[workspace["output"]][workspace["idx"]]["windows"][window["layout"]["pos_in_scrolling_layout"][0]][window["layout"]["pos_in_scrolling_layout"][1]]["id"] = window["id"]
            info[workspace["output"]][workspace["idx"]]["windows"][window["layout"]["pos_in_scrolling_layout"][0]][window["layout"]["pos_in_scrolling_layout"][1]]["app_id"] = str(window["app_id"])
            info[workspace["output"]][workspace["idx"]]["windows"][window["layout"]["pos_in_scrolling_layout"][0]][window["layout"]["pos_in_scrolling_layout"][1]]["is_urgent"] = window["is_urgent"]

        result = ""
        for output in info:
            result = result + f"\x1fO{output}\x1f"
            for workspace_idx, workspace in sorted(info[output].items()):
                if not workspace["is_active"]:
                    if workspace["is_urgent"]:
                        result = result + f"\x1fF8\x1f  {get_icon(workspace_idx)}  \x1fF\x1f"
                    elif "windows" in workspace:
                        result = result + f"\x1fF7\x1f  {get_icon(workspace_idx)}  \x1fF\x1f"
                else:
                    if "windows" not in workspace:
                        result = result + f"\x1fF1\x1f  {get_icon(workspace_idx)}  \x1fF\x1f"
                    else:
                        for _, column in sorted(workspace["windows"].items()):
                            for _, window in sorted(column.items()):
                                if window["id"] == workspace["active_window_id"]:
                                    result = result + f"\x1fF1\x1f\x1fB4\x1f  {get_icon(window["app_id"])}  \x1fF\x1f\x1fB\x1f"
                                elif window["is_urgent"]:
                                    result = result + f"\x1fF8\x1f  {get_icon(window["app_id"])}  \x1fF\x1f"
                                else:
                                    result = result + f"\x1fF1\x1f  {get_icon(window["app_id"])}  \x1fF\x1f"
            result = result + f"\x1fO\x1f"
        print(f"\x1fT1\x1f{result}\x1fT\x1f", flush=True)
