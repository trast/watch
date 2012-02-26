#!/usr/bin/python

import sys
import time
import os
import os.path
from watchdog.observers.api import BaseObserver, DEFAULT_EMITTER_TIMEOUT, DEFAULT_OBSERVER_TIMEOUT
from watchdog.observers.inotify import InotifyEmitter
from watchdog.events import FileSystemEventHandler
from watchdog.utils import DaemonThread
import socket
import fnmatch

eu = os.path.expanduser

lru = []

ignore = [
    '~/Mail',
    '~/News',
    '~/musik',
    '~/filme',
    '*/.git',
    '*/.svn',
    '~/.*',
    '*/tiles',
    '~/dl/radar',
    '~/g1/maps/*',
    '~/eth/vc_simulation/*',
    '~/logs',
    ]
ignore = [eu(x) for x in ignore]

class MyInotifyEmitter(InotifyEmitter):
    def __init__(self, event_queue, watch, timeout=DEFAULT_EMITTER_TIMEOUT):
        InotifyEmitter.__init__(self, event_queue, watch, timeout)
        for root,dirs,files in os.walk(watch.path):
            for i,d in reversed(list(enumerate(dirs))):
                fulldir = os.path.join(root,d)
                if not os.path.islink(fulldir) and \
                        not any(fnmatch.fnmatch(fulldir, pat) for pat in ignore):
                    print fulldir
                    self._inotify.add_watch(fulldir)
                else:
                    del dirs[i]

class MyInotifyObserver(BaseObserver):
    """
    Observer thread that schedules watching directories and dispatches
    calls to event handlers.
    """

    def __init__(self, timeout=DEFAULT_OBSERVER_TIMEOUT):
        BaseObserver.__init__(self, emitter_class=MyInotifyEmitter,
                              timeout=timeout)

HOME = eu('~')

class MyHandler(FileSystemEventHandler):
    def dispatch(self, event):
        global lru
        path = os.path.dirname(event.src_path)
        if path == HOME:
            return
        if path.startswith(HOME+'/'):
            path = path[len(HOME)+1:]
        if path not in lru:
            lru = [path] + lru[:4]
        else:
            lru.remove(path)
            lru = [path] + lru

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
SOCKNAME = "/home/thomas/.watchsock"

try:
    os.remove(SOCKNAME)
except OSError:
    pass

sock.bind(SOCKNAME)

if __name__ == "__main__":
    event_handler = MyHandler()
    observer = MyInotifyObserver()
    observer.schedule(event_handler, eu('~'))
    observer.start()
    try:
        while True:
            sock.listen(10)
            conn, addr = sock.accept()
            conn.send('\n'.join(lru) + '\n')
            conn.close()
    except KeyboardInterrupt:
        observer.stop()
    observer.join()
