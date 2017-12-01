tw6869
======

Linux driver for TW6869-based video capture cards

This is based on the Techwell/Intersil GPL driver that is floating around the net.  It has been modified to clean up the code, reduce debugging spew into the system log, and make it build on newer kernel versions (tested on 4.4.0).

I have added two example files "LCDd.conf" and "ip.pl"

LCDd.conf is an example file for the LCDd configuration. This will require tuning for your application

ip.pl is a perl script to display the IP address of a single interface. I am not fluent in perl, so this is a modified verison of my best attempt to nemd the tail.pl example (included with the LCDd source code) to my will. 
