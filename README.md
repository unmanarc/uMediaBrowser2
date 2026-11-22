# uMediaBrowser 

Unmanarc's Media Browser — a lightweight web file explorer & media gallery
  
Author: Aaron Mizrachi (unmanarc) <aaron@unmanarc.com>   
Main License: AGPL

***
## Installing packages (HOWTO)

- COPR (Fedora/CentOS/etc):  
[![Copr build status](https://copr.fedorainfracloud.org/coprs/amizrachi/unmanarc/package/uMediaBrowser/status_image/last_build.png)](https://copr.fedorainfracloud.org/coprs/amizrachi/unmanarc/package/uMediaBrowser/)


### Simple installation guide for RHEL:

To activate our repo's and download/install the software:

In RHEL7:
```bash
# Install EPEL Repo + COPR
yum -y install epel-release
yum -y install yum-plugin-copr

# Install unmanarc's copr
yum copr enable amizrachi/unmanarc -y
yum -y install uMediaBrowser
```

In RHEL8:
```bash
# Install EPEL Repo
dnf -y install 'dnf-command(config-manager)'
dnf config-manager --set-enabled powertools
dnf -y install epel-release

# Install unmanarc's copr
dnf copr enable amizrachi/unmanarc -y
dnf -y install uMediaBrowser
```


***
## Project Description

Lightweight web file explorer and media gallery that comes with:

- Password Protected Authentication
- TLS (https) Support
- On-the-fly tar.gz generation
- Web file manager UI with [keyboard shortcuts](shortcuts.md)
- Timeline with events spanning folders, progressive loading, and a shared media carousel

### API key authentication

Set a non-empty `APIKEY` environment variable before starting the server to
require the exact same value in the `API-Key` header on every request.
Missing or incorrect keys return HTTP 401. An unset or empty `APIKEY` disables
this requirement. If `--passfile` is also configured, both credentials are required.
Startup logs announce API key authentication without printing the key.

```bash
APIKEY='your-secret-key' ./build/timeline/uMediaBrowser2 --rootdir /path/to/media
curl -H 'API-Key: your-secret-key' 'http://localhost:8001/?act=ls'
```

### Timeline

Use the **Timeline** button in the folder browser, or open `/?view=timeline`.
The **Folders** button returns to the original browser. Both pages share their
styles, thumbnail workers, and full-screen viewer.

The timeline uses digiKam's `digikam4.db` through the **libMantids30 SQLite3**
connector. Set the served directory with `--rootdir`, for example:

```bash
./build/timeline/uMediaBrowser2 --rootdir /home/user/Pictures/Albums/Familiar
```

Starting at that directory, the server searches its parents up to `/` and opens
the nearest `digikam4.db` with SQLite's **read-only** `mode=ro`. SQLite must be
built with URI support (`USE_URI`). The database is never created or modified.
`AlbumRoots` and `Albums` resolve collection paths; only entries inside
`--rootdir` enter the timeline. Collection identifiers with `path=` (including
percent-encoded paths) and absolute `specificPath` values are supported.

Timeline dates use the first valid source in this order:

1. A capture date embedded in an `IMG` filename, such as
   `IMG_20170706-125641_001_.jpg` or `IMG_20240229_230405_Canon.CR3`.
2. **`ImageInformation.creationDate`**, already extracted by digiKam.
3. **`Images.modificationDate`**.

The filename patterns are case-insensitive and anchored at the beginning:
`^IMG[_-]([0-9]{8})[-_]([0-9]{6})($|[^0-9])`,
`^IMG[_-]([0-9]{8})-WA[0-9]`, and `^IMG[_-]([0-9]{8})($|[.])`.
Date-only names such as `IMG-20140128-WA0002.jpg` and `IMG_20240229.jpg`
use midnight. Camera counters such as `IMG_1234.JPG`, malformed patterns and
impossible dates fall through to the next source. SQLite implements the pattern
checks using GLOB digit classes and fixed positions, without a REGEXP extension.

File sizes come from `Images.fileSize`. Dates without an explicit timezone use
server local time; explicit offsets are respected. There is no recursive media
scan or EXIF extraction for the timeline. JSON exposes `date_source` as
`filename`, `creationDate` or `modificationDate`; `mtime` retains the original
modification date (or 0 if unavailable).

`TimelineDatabase` supplies feed pages, event pages and random samples through
request-local read-only transactions. SQL joins the digiKam tables, constrains
paths to `--rootdir`, converts dates, orders the results and filters event time
ranges. Rows are consumed one at a time; only the requested page and up to six
samples per displayed event are retained. There is no application media index,
snapshot cache or startup scan. SQLite may use temporary storage for sorting.

Exact existing-file totals and configured event boundaries require streaming the
matching ordered rows and checking file existence. Missing files, symlinks and
entries without valid dates are skipped before counting or pagination. Adjacent
selected capture dates belong to the same event while the pause between adjacent
images is **less than 30 minutes** by default, regardless of their folders.
A pause of exactly 30 minutes or longer starts a new group. Configure this
threshold with a positive integer number of minutes:

```bash
./build/timeline/uMediaBrowser2 --rootdir /home/user/Pictures/Albums/Familiar --timeline-gap-minutes 30
```

The gap is measured from the previous image, not from the start of the group. If the database is missing or cannot be read, the
timeline shows an error and the folder browser continues to work as usual.

The feed initially requests 8 events. Each story shows at most 6 random thumbnails;
one visible story refreshes its sample every 12 seconds. Scrolling or **More +**
loads older events. **Open event** shows a gallery with folder separators, grid/list
controls and 60 items per page. The carousel can traverse the whole event,
including files beyond the gallery's loaded pages.

Every request reads the current database; new digiKam entries and file removals
do not require rebuilding an index or restarting the server. New files appear in
the timeline after digiKam has indexed them; the folder browser shows them
immediately. Uploads, deletions and the refresh button advance the client revision.
Database/WAL file identity also detects external changes and replacement. An
outdated pagination token returns HTTP 409 so the UI can reload current results.
Event IDs contain a revision and time bounds for direct event range queries.

The database layer and its HTTP adapter include Doxygen comments describing the
API, ownership, memory bounds, read-only guarantees, ordering and error handling.
Generate HTML reference documentation with `doxygen Doxyfile.timeline` (requires
Doxygen); the output is written under `build/docs/timeline/html/`.

Additional JSON services (same authentication as the folder browser):

| Request | Result |
| --- | --- |
| `GET ?act=timeline&cursor=0&limit=8` | Newest events, samples, revision and next cursor; pass `revision` on subsequent pages |
| `GET ?act=timeline_event&event=ID&cursor=0&limit=60` | Event items ordered by folder, with global event positions |
| `GET ?act=timeline_samples&event=ID` | Up to six random items from an event |
| `POST ?act=timeline_refresh` | Advance the client revision and read the first page; requires the usual CSRF cookie/header |

Integration tests create a temporary library and launch a local server. They
require `convert` and `node` in addition to the built application:

```bash
python3 tests/test_timeline.py build/timeline/uMediaBrowser2
```

### Use Cases:

This program is useful for:

- Creating Backups (tar.gz) of folders on-the-fly
- Serving files via HTTP efficiently

It can be built with a small runtime footprint and introduced as a very lightweight binary  

***
## Building/Installing uMediaBrowser

### Building Instructions:

This guide is optimized for centos7 (you can adapt this for your OS), and the generated binary should be compatible with other neewer distributions...

First, as prerequisite, you must have installed libMantids30 (>= 3.0.0) and its pkg-config files.

and then...

```
git clone https://github.com/unmanarc/uMediaBrowser
cd uMediaBrowser
PKG_CONFIG_PATH=/opt/osslibs/share/pkgconfig:$PKG_CONFIG_PATH \
cmake -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX:PATH=/opt/osslibs -DEXTRAPREFIX:PATH=/opt/osslibs -B../uMediaBrowser-build .
cmake --build ../uMediaBrowser-build -j4
cmake --install ../uMediaBrowser-build
```

And if you want to reduce the binary size:

```
strip -x -X -s /opt/osslibs/bin/uMediaBrowser
upx -9 --ultra-brute /opt/osslibs/bin/uMediaBrowser
```

The resulting expected binary size should be: **~200K** without https support (yes, it's still a dinosaur, but at least it will fit in your 5 1/4 floppy disk)


### Running Instructions:

Program help:

```
$ /opt/osslibs/bin/uMediaBrowser -h
# Unmanarc's Media Browser (uMediaBrowser) v1.0.3 alpha
# Author:  Aarón Mizrachi (aaron@unmanarc.com)
# License: AGPL
# 

Help:
-----

HTTP Options:
-------------
-r --rootdir <value> : HTTP Document Root Directory (default: .)
-l --lport   <value> : Local HTTP Port (default: 8001)
-4 --ipv4            : Use only IPv4 (default: true)
-a --laddr   <value> : Listen Address (default: *)
-t --threads <value> : Max Concurrent Connections (Threads) (default: 1024)
    --use-x-real-ip : Use X-Real-IP header for client IP in logs instead of the source IP (behind proxy) (default: false)

HTTP Security:
--------------
-p --passfile <value> : HTTP User/Pass File (default: )

Other Options:
--------------
-v --verbose <value> : Set verbosity level. (default: 0)
-h --help            : Show information usage. (default: false)
-s --sys             : Journalctl Log Mode (don't print colors or dates) (default: false)

Server Options:
---------------
-g --can-archive     : Allow to get server directories as tar.gz files (on-the-fly) (default: false)
-u --can-upload      : Allow file uploads (default: false)
-o --can-overwrite   : Allow overwrite on file uploads and/or delete files (default: false)

Service Options:
----------------
-d --daemon         : Run as daemon. (default: false)
```

**Security Considerations:**

1. HTTP Communication can be eavesdropped or tampered, if you set up an HTTP web server, at least use a layer of encryption like a VPN, TOR or something...
2. For passfile, you should create a file where the first line is the user, and the second line is the password in clear text.

**For HTTPS Only:**  
1. Remember to set up private signed and trusted X.509 Certificates, if not, the communication can be eavesdropped or tampered!!!



### Service Creation Instructions:

- Create the services...
- Restart daemon
```
cat << 'EOF' | install -m 640 /dev/stdin /usr/lib/systemd/system/uMediaBrowser.service
[Unit]
Description=Unmanarc's Media Browser
After=network.target

[Service]
Type=simple
Restart=always
RestartSec=1
ExecStart=/opt/osslibs/bin/uMediaBrowser -s -r /isos
#User=
#Group=

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now uMediaBrowser.service
```

`Don't forget to change the HTTP Document Root directory (eg. /isos) and change the User=/Group= for your specific user/group combination`


***
## Compatibility

This program was tested so far in:

* Fedora Linux 34
* Ubuntu 20.04 LTS (Server)
* CentOS/RHEL 7/8

### Overall Pre-requisites:

* libMantids30 (>= 3.0.0)
* cmake >= 3.12
* C++17 Compatible Compiler (like GCC >= 7)
