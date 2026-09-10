# Stoop

Stoop is a solar-powered LoRa mesh node that runs a local bulletin board over WiFi. One node serves everyone nearby with a phone or computer. Built on [MeshCore](https://github.com/meshcore-dev/MeshCore) as the mesh transport.

## The idea

Existing LoRa bulletin boards (MeshCore room servers, Meshtastic BBS) require all the users to own a radio. Stoop puts the radio at a fixed location and serves phones over WiFi instead. There's nothing to install and no account to create. connect to the WiFi and use a browser to read or post.

## How it works

Each node is an ESP32-S3 broadcasting an open WiFi access point with a captive portal. Phones connect, see the board, and can read or post without installing anything. Posts made locally are stored on the node and forwarded to other Stoop nodes over LoRa. Reads never touch the radio; only writes do.

The mesh has very little bandwidth to spend. The whole network can carry a few thousand SMS-length posts a day. This behaves like a village noticeboard with a strict word limit: post length is capped, and every post competes for the same shared airtime.

## Trust model

**Phone to node.** WiFi is open, there are no accounts, and a username is just a label someone typed in, with nothing behind it. Anyone in range can post as anyone, but the username is appended with the site name.

**Node to node.** The main stoop network is a private network to the nodes and is encrypted by the standard meshcore transport. MeshCore channels encrypt traffic but don't attribute a message to a sender, so attribution happens at the application layer instead of being inherited from the transport.

A post renders as `name@node`. The node half is static. The name half is just text someone typed. Forging another node's identity is generally not possible. Standing at a node and posting under a neighbor's name is possible, and that's an accepted tradeoff for a system with no accounts considering imposters will be sharing the same physical space.

The channel itself uses a private, randomly generated secret rather than one derived from its name, and rotating that secret is a routine config change.

## Rate limits

There are a lot of people on Meshcore in SoCal. In order to be respectful of the airspace, rate limits for both users and nodes have been implemented on the firmware level. It's built on a bucket system so a user starts with a max number of messages (5) and gets a new one back every 30 seconds. The node operates on a similar system.

## Staying useful on a normal day

A tool that only works during a disaster has no users on day one of that disaster, because nobody has connected to it before. Stoop nodes are meant to carry routine, low-stakes content too, so people already know the SSID and the hardware has been exercised before it matters.

## Constraints worth knowing about

- No internet connection, ever, so no CDNs and no external assets. Everything the browser needs ships inside the firmware.
- No TLS, so nothing sensitive should be typed into this browser.
- Only a handful of WiFi clients can be connected at once, around 4 to 8.
- Pages render inside the captive portal WebView on iOS, so plain HTML and light JavaScript, not a framework.
- Web assets are gzipped and embedded into the firmware at build time; see `bin/embed_web.py`.

## Building

This is a PlatformIO project. The current Stoop firmware target is `heltec_v4_stoop_radio`:

```
pio run -e heltec_v4_stoop_radio
```

For active development, `stoop_dev` builds the same firmware with debug logging enabled and uploads plus opens a serial monitor automatically:

```
pio run -e stoop_dev
```

To make the `stoop` channel private, copy `platformio.local.ini.template` to `platformio.local.ini` (already gitignored) and set your own channel key there.

## Repo layout

Stoop is a fork of MeshCore and stays mergeable with upstream on purpose. All Stoop-specific code lives under `examples/stoop_radio/`; shared MeshCore source under `src/` is not modified.

```
examples/stoop_radio/
  main.cpp             setup/loop, WiFi AP, captive portal, HTTP routes
  MyMesh.cpp/.h         mesh behavior for this node
  RateLimiter.cpp/.h    the two token buckets described above
  web/                  the HTML and JS served to phones
```

## Status

A user can connect to the WiFi hotspot and is taken to the main page by a captive portal. The user can specify a username and send messages over the private stoop channel. They can search through previous messages. 

## ToDo
- need to add an SD card to persist messages through reboots and add the ability to save 100s or 1000s of messages without competing with ESP flash.
- need to stress test the system with multiple phones connected and sending messages.
- want to add another node type as a weather station or other types of admin data that can be published to the network.
- should add some functionality to kick users off the network if they've been there awhile to make room for others.