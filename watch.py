#!/usr/bin/python

import sys
import time
import os
import os.path
import re
import socket
import fnmatch
import pyinotify
from watchdog.utils import DaemonThread

eu = os.path.expanduser
HOME = eu('~')

wm = pyinotify.WatchManager()
mask = pyinotify.ALL_EVENTS

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
ignore_re = re.compile('|'.join('(?:%s)' % fnmatch.translate(eu(x)) for x in ignore))

def setup_watches(path):
    for root,dirs,files in os.walk(path):
        for i,d in reversed(list(enumerate(dirs))):
            fulldir = os.path.join(root,d)
            if not os.path.islink(fulldir) and not ignore_re.match(fulldir):
                print fulldir
                wm.add_watch(fulldir, mask)
            else:
                del dirs[i]

class Handler(pyinotify.ProcessEvent):
    def process_normal(self, event):
        path = os.path.join(event.path, event.name)
        print "see_dirname: %s" % path
        self.see_dirname(path)
    process_IN_CREATE = process_normal
    process_IN_DELETE = process_normal
    process_IN_ACCESS = process_normal
    def see_dirname(self, path):
        self.see(os.path.dirname(path))
    def see(self, path):
        global lru
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

class NotifierThread(DaemonThread):
    def __init__(self):
        DaemonThread.__init__(self)
        self.notifier = pyinotify.Notifier(wm, Handler())
    def run(self):
        while self.should_keep_running():
            self.notifier.process_events()
            if self.notifier.check_events():
                self.notifier.read_events()

if __name__ == "__main__":
    setup_watches(HOME)
    notifier = NotifierThread()
    notifier.start()
    try:
        while True:
            sock.listen(10)
            conn, addr = sock.accept()
            conn.send('\n'.join(lru) + '\n')
            conn.close()
    except KeyboardInterrupt:
        notifier.stop()
    notifier.join()
