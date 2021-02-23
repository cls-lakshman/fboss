#!/usr/bin/env python3
#
#  Copyright (c) 2004-present, Facebook, Inc.
#  All rights reserved.
#
#  This source code is licensed under the BSD-style license found in the
#  LICENSE file in the root directory of this source tree. An additional grant
#  of patent rights can be found in the PATENTS file in the same directory.
#

import re
import socket
import sys
import typing as t
from collections import defaultdict
from typing import DefaultDict, Dict, List, Tuple

from facebook.network.Address.ttypes import BinaryAddress
from libfb.py.decorators import retryable
from neteng.fboss.ctrl.ttypes import NextHopThrift
from neteng.fboss.mpls.ttypes import MplsAction, MplsActionCode


AGENT_KEYWORD = "agent"
BGP_KEYWORD = "bgp"
COOP_KEYWORD = "coop"
SDK_KEYWORD = "sdk"
SSL_KEYWORD = "ssl"

KEYWORD_CONFIG = "config"

KEYWORD_CONFIG_SHOW = "show"
KEYWORD_CONFIG_RELOAD = "reload"


def get_colors() -> Tuple[str, str, str]:
    if sys.stdout.isatty():
        return ("\033[31m", "\033[32m", "\033[m")
    return ("", "", "")


def ip_to_binary(ip):
    for family in (socket.AF_INET, socket.AF_INET6):
        try:
            data = socket.inet_pton(family, ip)
        except socket.error:
            continue
        return BinaryAddress(addr=data)
    raise socket.error("illegal IP address string: {}".format(ip))


def ip_ntop(addr):
    if len(addr) == 4:
        return socket.inet_ntop(socket.AF_INET, addr)
    elif len(addr) == 16:
        return socket.inet_ntop(socket.AF_INET6, addr)
    else:
        raise ValueError("bad binary address %r" % (addr,))


def port_sort_fn(port):
    if not port.name:
        return "", port.portId, 0, 0
    return port_name_sort_fn(port.name)


def port_name_sort_fn(port_name):
    m = re.match(r"([a-z][a-z][a-z])(\d+)/(\d+)/(\d)", port_name)
    if not m:
        return "", 0, 0, 0
    return m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))


def make_error_string(msg):
    COLOR_RED, COLOR_GREEN, COLOR_RESET = get_colors()
    return COLOR_RED + msg + COLOR_RESET


def get_status_strs(status, is_present):
    """ Get port status attributes """

    COLOR_RED, COLOR_GREEN, COLOR_RESET = get_colors()
    attrs = {}
    admin_status = "Enabled"
    link_status = "Up"
    present = "Present"
    speed = ""
    profileID = getattr(status, "profileID", "")

    if status.speedMbps:
        speed = "{}G".format(status.speedMbps // 1000)
    padding = 0

    color_start = COLOR_GREEN
    color_end = COLOR_RESET
    if not status.enabled:
        admin_status = "Disabled"
        speed = ""
    if not status.up:
        link_status = "Down"
        if status.enabled and is_present:
            color_start = COLOR_RED
        else:
            color_start = ""
            color_end = ""
    if is_present is None:
        present = "Unknown"
    elif not is_present:
        present = ""

    if color_start:
        padding = 10 - len(link_status)
    color_align = " " * padding

    link_status = color_start + link_status + color_end

    attrs["admin_status"] = admin_status
    attrs["link_status"] = link_status
    attrs["color_align"] = color_align
    attrs["present"] = present
    attrs["speed"] = speed
    attrs["profileID"] = profileID if profileID else "-"

    return attrs


def get_qsfp_info_map(qsfp_client, qsfps, continue_on_error=False):
    if not qsfp_client:
        return {}
    try:
        return qsfp_client.getTransceiverInfo(qsfps)
    except Exception as e:
        if not continue_on_error:
            raise
        print(
            make_error_string("Could not get qsfp info; continue anyway\n{}".format(e))
        )
        return {}


@retryable(num_tries=3, sleep_time=0.1)
def get_vlan_port_map(
    agent_client, qsfp_client, colors=True, details=True
) -> DefaultDict[str, DefaultDict[str, List[str]]]:
    """ fetch port info and map vlan -> ports """
    all_port_info_map = agent_client.getAllPortInfo()
    port_status_map = agent_client.getPortStatus()

    qsfp_info_map = get_qsfp_info_map(qsfp_client, None, continue_on_error=True)

    vlan_port_map: DefaultDict[str, DefaultDict[str, List[str]]] = defaultdict(
        lambda: defaultdict(lambda: [])
    )
    for port in all_port_info_map.values():
        # unconfigured ports can be skipped
        if port.vlans is None or len(port.vlans) == 0:
            continue
        # fboss ports currently only support a single vlan
        assert len(port.vlans) == 1
        vlan = port.vlans[0]
        # root port is the parent physical port
        match = re.match(r"(?P<port>.*)\/\d+$", port.name)
        if match:
            root_port = match.group("port")
        else:
            sys.exit(f"root_port for {port.name} could not be determined")
        port_status = port_status_map.get(port.portId)
        enabled = port_status.enabled
        up = port_status.up
        speed = int(port_status.speedMbps / 1000)
        # ports with transceiver data

        fab_port = "fab" in port.name

        if port_status.transceiverIdx:
            channels = port_status.transceiverIdx.channels
            qsfp_id = port_status.transceiverIdx.transceiverId
        else:
            channels = []
            qsfp_id = None

        # galaxy fab ports have no transceiver
        qsfp_info = qsfp_info_map.get(qsfp_id)
        qsfp_present = qsfp_info.present if qsfp_info else False

        port_summary = get_port_summary(
            port.name,
            channels,
            qsfp_present,
            fab_port,
            enabled,
            speed,
            up,
            colors,
            details,
        )

        if not port_summary:
            continue

        vlan_port_map[vlan][root_port].append(port_summary)

    return vlan_port_map


def get_port_summary(
    port_name: str,
    channels: List[int],
    qsfp_present: bool,
    fab_port: bool,
    enabled: bool,
    speed: int,
    up: bool,
    details: bool,
    colors: bool,
) -> str:
    """ build the port summary output taking into account various state """
    COLOR_RED, COLOR_GREEN, COLOR_RESET = get_colors() if colors else ("", "", "")
    port_speed_display = get_port_speed_display(speed, enabled, up) if details else ""
    # port has channels assigned with sfp present or fab port, is enabled/up
    if ((channels and qsfp_present) or fab_port) and enabled and up:
        return f"{COLOR_GREEN}{port_name}{COLOR_RESET} {port_speed_display}"
    # port has channels assigned with sfp present or fab port, is enabled/down
    if ((channels and qsfp_present) or fab_port) and enabled and not up:
        return f"{COLOR_RED}{port_name}{COLOR_RESET} {port_speed_display}"
    # port has channels assigned with no sfp present, is enabled/up
    if (channels and not qsfp_present) and enabled:
        return f"{port_name} {port_speed_display}"
    # port is of no interest (no channel assigned, no SFP, or disabled)
    return ""


def get_port_speed_display(speed, enabled, up):
    if not enabled:
        return ""
    if enabled and not up:
        return "()"
    if enabled and up:
        return f"({speed}G)"

    raise RuntimeError(
        f"Invalid port state: speed - {speed}, enabled - {enabled}, up - {up}"
    )


@retryable(num_tries=3, sleep_time=0.1)
def get_vlan_aggregate_port_map(client) -> Dict[str, str]:
    """ fetch aggregate port table and map vlan -> port channel name"""
    aggregate_port_table = client.getAggregatePortTable()
    vlan_aggregate_port_map: Dict = {}
    for aggregate_port in aggregate_port_table:
        agg_port_name = aggregate_port.name
        for member_port in aggregate_port.memberPorts:
            vlans = client.getPortInfo(member_port.memberPortID).vlans
            assert len(vlans) == 1
            vlan = vlans[0]
            vlan_aggregate_port_map[vlan] = agg_port_name
    return vlan_aggregate_port_map


def label_forwarding_action_to_str(label_forwarding_action: MplsAction) -> str:
    if not label_forwarding_action:
        return ""
    code = MplsActionCode._VALUES_TO_NAMES[label_forwarding_action.action]
    labels = ""
    if label_forwarding_action.action == MplsActionCode.SWAP:
        labels = ": " + str(label_forwarding_action.swapLabel)
    elif label_forwarding_action.action == MplsActionCode.PUSH:
        stack_str = "{{{}}}".format(
            ",".join([str(element) for element in label_forwarding_action.pushLabels])
        )
        labels = ": {}".format(stack_str)

    return " MPLS -> {} {}".format(code, labels)


def nexthop_to_str(
    nexthop: NextHopThrift,
    vlan_aggregate_port_map: t.Dict[str, str] = None,
    vlan_port_map: t.DefaultDict[str, t.DefaultDict[str, t.List[str]]] = None,
) -> str:
    weight_str = ""
    via_str = ""
    label_str = label_forwarding_action_to_str(nexthop.mplsAction)

    if nexthop.weight:
        weight_str = " weight {}".format(nexthop.weight)

    nh = nexthop.address
    if nh.ifName:
        if vlan_port_map:
            vlan_id = int(nh.ifName.replace("fboss", ""))

            # For agg ports it's better to display the agg port name,
            # rather than the phy
            if vlan_id in vlan_aggregate_port_map.keys():
                via_str = vlan_aggregate_port_map[vlan_id]
            else:
                port_names = []
                for ports in vlan_port_map[vlan_id].values():
                    for port in ports:
                        port_names.append(port)

                via_str = ", ".join(port_names)
        else:
            via_str = nh.ifName

    if via_str:
        via_str = f" dev {via_str.strip()}"
    return f"{ip_ntop(nh.addr)}{via_str}{weight_str}{label_str}"
