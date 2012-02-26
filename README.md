watch
=====

(The name will be changed eventually, but it's not the biggest problem right now :-)

### Description

_watch_ attempts to keep track of your most recently used directories.  To this end it uses the inotify API (thus Linux only) to monitor your filesystem for pretty much any event, and whenever you do anything to a file/directory, it puts the relevant directory into a short history list.

It is inspired by [Fresh](http://www.ironicsoftware.com/fresh/index.html) which was advertised on [One Thing Well](http://onethingwell.org/post/18073090299/fresh).  However since that is OS X only, I could not even run it to have a look.

Thinking about the idea, I noticed that I frequently want to open a shell to play with files I have open in some other program (e.g., after opening a file in Emacs I frequently want to have a shell to use git in the same directory).  Of course, opening a shell does not require knowing the _file_ path, just the directory, hence this tool.

### Usage

* Edit the source: at the very least you need to s|/home/thomas|your_home_directory|
* `make`; you may want to optimize as well
* Run it
* Do something with its output :-)

The "output" is in ~/.watchsock.  Whenever you connect to that, it will send you the most-recently-used list (and close the connection immediately).  So you can do things like

    # see the list; note -d is required with netcat
    nc -dU ~/.watchsock

    # put this in your ~/.bashrc to have 1..5 aliased to 'cd $dir'
    _recent=$(nc -dU ~/.watchsock)
    if [ $? = 0 -a -n "$_recent" ]; then
	echo "Recent directories:"
	echo "$_recent" | nl
	eval "$(echo "$_recent" | perl -ne 'chomp;print "alias $.=\"cd $_\"\n"')"
    fi

Or for a fancier usage, I have the following in my xmonad.hs, with a key bound to `recentDirPrompt myXPConfig`.  Pressing the key pops up a choice of recently-used dirs, and it will start a konsole in the one I pick:

    import XMonad.Util.Run
    import XMonad.Prompt
    import XMonad.Prompt.Shell (Shell)

    getRecentDirs :: IO [String]
    getRecentDirs = fmap lines $ runProcessWithInput "nc" ["-dU", "/home/thomas/.watchsock"] ""

    data Dirs = Dirs

    instance XPrompt Dirs where
        showXPrompt Dirs      = "Dir: "

    recentDirPrompt :: XPConfig -> X ()
    recentDirPrompt c = do
        cmds <- io getRecentDirs
        mkXPrompt Dirs c (mkComplFunFromList' cmds) spawnKonsole

    spawnKonsole :: String -> X ()
    spawnKonsole dir = spawn $ "konsole --workdir " ++ dir

I'm sure you have more great ideas.

### TODO

* Make the hard-coded values configurable
* Fix the wait when interrupting (right now it only exits if the inotify-reading thread has something to read)
* Give more interesting example usages
* Optionally track files instead of dirs?
