Clip-Boy Evil Portal - EXAMPLE FILES
====================================

These are TEMPLATE / EXAMPLE files, shipped on the Research build only. They are
harmless demonstrations - as shipped they capture NOTHING.

WHAT THIS TOOL IS
-----------------
Evil Portal broadcasts a WiFi access point and serves a web page to anyone who
joins it (a "captive portal"). It is included for AUTHORIZED security testing,
red-team training, and education.

LEGAL - READ THIS
-----------------
Running a captive portal that captures another person's input, or that
impersonates a network you do not own, may be illegal without explicit written
authorization. Only run this against networks and users you own or have written
permission to test. You are responsible for how you use it. See the Legal screen
on the badge (DATA > Settings > Legal).

FILES
-----
  ap.config.txt   The AP (WiFi network) name the portal broadcasts. One line.
  index.html      The page served to anyone who joins. This example is a plain
                  notice with NO capture form.

HOW TO USE (deliberate steps)
-----------------------------
1. Copy  index.html  and  ap.config.txt  to the ROOT of the SD card
   (so they are at  /index.html  and  /ap.config.txt ).
2. Edit  ap.config.txt  to a name you are authorized to use.
3. Edit  index.html  to build your authorized test page. To capture submitted
   input you must DELIBERATELY add a form that posts to /get - the example ships
   with that form commented out on purpose.
4. On the badge: DATA > Tools > Evil Portal > Start.
5. The badge serves the page at http://172.0.0.1/ to any client that joins.

These examples live in the badge's internal storage and were copied to this SD
folder so you can edit them on a computer. Editing them here does not change the
built-in copies.
