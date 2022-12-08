# smol_http
---
A simple http server supporting GET requests.

# Usage
-----

Clone this repository and compile the software.

```sh
git clone https://github.com/elttil/smol_http
cd smol_http
make
make install # This will install smol_http to /usr/bin
```

The applications default values can be configured via changing the values in
the config.h file and recompiling the application.

It is also possible to change values using the command line arguments

* -p port

* -d root directory

# Security
---

The application is a seteuid binary and must not be ran as root but instead
should be ran as a low privilege user that the application will later drop
privileges to.

# Attribution
---

Attribution is under no circumstance required but is appreciated. Read
LICENSE for more information.
