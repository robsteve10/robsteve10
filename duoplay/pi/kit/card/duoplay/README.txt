DuoPlay for Raspberry Pi
========================

This card runs DuoPlay on a Raspberry Pi Zero W (a Zero 2 W, 3, 2 or 1 works too).
It's Alpine Linux, running from memory: pulling the plug is fine.

FIRST START
1. Put the card in the Pi and plug it in. Give it a minute.
2. On your iPhone: Settings > Wi-Fi > join "DuoPlay Setup".
   A page opens by itself. Choose your home Wi-Fi (the one your PS5 is on) and type its password.
3. Back on your own Wi-Fi, open Safari and go to   http://duoplay.local:8080
   Then tap Share > Add to Home Screen, and open DuoPlay from your Home Screen.

THE GREEN LIGHT ON THE PI
  steady            ready
  blinking fast     joining your Wi-Fi
  blinking slowly   the "DuoPlay Setup" network is up, waiting for you

CHANGING WI-FI
If the Pi can't find its network (new router, new home), "DuoPlay Setup" comes back by
itself after about a minute. To start over, delete duoplay/wifi.conf from this card.

Rather type it in? Put a file named wifi.txt at the top of this card (next to this folder):
    name: Your Network
    password: your password
    country: US
The Pi reads it once, then deletes it so your password isn't left on the card.

FASTER
A USB Ethernet adapter (with a micro-USB OTG cable) is used automatically when plugged in.
Wired, the Pi's Wi-Fi only has to carry the picture once instead of twice.

WHAT'S WHERE
  duoplay/duoplay-bridge   the DuoPlay program (replace it to update)
  duoplay/wifi.conf        the Wi-Fi network the Pi joins
  duoplay/data/            pairing with your PS5
  Everything else is Alpine Linux (alpinelinux.org), unmodified.
