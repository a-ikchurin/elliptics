# =============================================================================
# 2013+ Copyright (c) Kirill Smorodinnikov <shaitkir@gmail.com>
# All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# =============================================================================

from elliptics import log_level
from elliptics import record_flags
from elliptics.core import *
from elliptics.node import Node
from elliptics.route import Address

import warnings

warnings.simplefilter('always', PendingDeprecationWarning)


def dump_record_flags(flags):
    return '|'.join(name for name, flag in record_flags.names.iteritems() if flags & flag)


@property
def storage_address(self):
    """
    Node address as elliptics.Address
    """
    return Address.from_host_port(self.__storage_address__)


@property
def monitor_statistics(self):
    from json import loads
    return loads(self.__statistics__)


def wrap_address(classes):
    @property
    def address(self):
        """
        Node address as elliptics.Address
        """
        return Address.from_host_port(self.__address__)
    for cls in classes:
        cls.__address__ = cls.address
        cls.address = address

LookupResultEntry.__storage_address__ = LookupResultEntry.storage_address
LookupResultEntry.storage_address = storage_address

MonitorStatResultEntry.__statistics__ = MonitorStatResultEntry.statistics
MonitorStatResultEntry.statistics = monitor_statistics

wrap_address([CallbackResultEntry,
              RouteEntry
              ])


def create_node(elog=None, log_file='/dev/stderr', log_level=log_level.error,
                cfg=None, wait_timeout=3600, check_timeout=60,
                flags=0, io_thread_num=1, net_thread_num=1,
                nonblocking_io_thread_num=1, remotes=[], log_watched=False):
    if not elog:
        elog = Logger(log_file, log_level, log_watched)

    if not cfg:
        cfg = Config()
        cfg.wait_timeout = wait_timeout
        cfg.check_timeout = check_timeout
        cfg.flags = flags
        cfg.io_thread_num = io_thread_num
        cfg.nonblocking_io_thread_num = nonblocking_io_thread_num
        cfg.net_thread_num = net_thread_num

    n = Node(elog, cfg)
    try:
        n.add_remotes(remotes)
    except:
        pass
    return n


del storage_address
del wrap_address
