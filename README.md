# emajail

Simple user space utility to jail processes; main purpose of this is to have a *fair* sense of safety when running not fully trusted binaries on linux.

## Building

Just download this repository and invoke `make` (`make release` for optimized build). This project does not depend on any library, only a recent kernel and compiler (c++11) are needed.

## Running

### All commands

```
./emajail --help
[3058]	2017-07-23 19:02:03.832	(INFO)	Starting emajail ...
Usage: ./emajail [options] cmd [arg...]

Create a child process in a sandboxed environment
without modifying any existing file (using overlayFS)

Options can be:

--empty-home              Sets up an empty /home on 'tmpfs'

--empty-proc              Sets up an empty /proc on 'proc'. Please note
                          this will set the flag CLONE_NEWPID and implies
                          software may not work (PulseAudio, ...)

-h,--help                 Print this screen and quit

-j,--jail                 Quick combination as specifying the three options
                          --empty-home, --empty-proc and -s

-o,--overlay-dir path     Use a fixed path for overlayFS
                          (otherwise on /dev/shm/emajail_XXXXXX)

--skip-dirs               Print directories to not overlay and quit

--silent                  Do not print any logline

-s,--strict               Add IPC isolation level and create a new PID group; this
                          option might imply some software to not work or
                          fail at unexpected points, but increases security
                          levels greatly

```

### Sample usage

This will spawn a process and create a new file which will be visible in just current session:

```
./emajail vim ./wontstay.txt
```

This will instead spawn a very stand alone process:

```
./emajail -j glxgears
```

## F.A.Q.

### Why did you create this?

Wanted to use *firejail* but it wasn't straightforward - I wanted to have a *full* read-only filesystem, but wasn't that easy with that software: so I decided to write somethign simple to just do the job.

### The code seems much less than *firejail*...

It is much less - this software doesn't pretend to be such a comprehensive solution as *firejail*, but is definitely stricter than it and lets me/us explore with Linux *overlay-fs* and *newspaces* in a very simple way.

### I want to extend it

This is open source, you can do what you want under the GPL3 - also send me pull requests!

### I want feature *x*

Let's discuss - but also, you have the code, go on and have fun, then perhaps raise a pull request!

## License

```
	emajail (C) 2017 E. Oriani, ema <AT> fastwebnet <DOT> it

	This file is part of emajail.

	emajail is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	emajail is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with emajail.  If not, see <http://www.gnu.org/licenses/>.
```

## Credits

Thanks to Linux for being open source and to:
- [firejail](https://firejail.wordpress.com/) For [not working as expected](https://superuser.com/questions/1155653/is-it-expected-that-firejail-allows-r-w-outside-of-the-sandbox-without-overla), being complex and thus inspiring me to write a simpler applet
- [overlay-fs](https://www.kernel.org/doc/Documentation/filesystems/overlayfs.txt) For making this happen
