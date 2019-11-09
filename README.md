# Compositor-killer

A malicious example Wayland client, designed to take a very long time to
complete its drawing.  Because of implicit synchronization, this can bring the
Wayland compositor to a crawl. Although implicit synchronization affects X11 too.

This is done by drawing a very large number of iterations of the Mandlebrot set.

### Options

- `-i <n>`: Number of iterations of the Mandlebrot set. Default: 1000.
- `-f <w>x<h>`: Run at a fixed size with these dimensions.
- `-l <n>`: Draw this many frames before quitting.
- `-u`: Run unsynchronized, i.e. ignore wl_surface.frame events.
