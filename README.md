# esplora_clnd_plugin

Build status: [![Build Status](https://travis-ci.org/lvaccaro/esplora_clnd_plugin.png?branch=master)](https://travis-ci.org/lvaccaro/esplora_clnd_plugin)

[c-lightning](https://github.com/ElementsProject/lightning) C plugin to use (as possible) [esplora web explorer](https://blockstream.info) to fetch bitcoin data.

1st c-lightning plugin with esplora integration is [sauron plugin](https://github.com/lightningd/plugins/tree/master/sauron) developped by [darosior](https://github.com/darosior).

#### Build

1. copy `esplora.c` into `lightning/plugins` folder
2. apply `Makefile.patch` to add esplora in plugins build (tested for clightning v0.9.0)
```
patch -p1 < Makefile.patch
```
3. edit main `lightning/Makefile` as the following:
- add esplora plugin to PLUGIN list
```
PLUGINS=plugins/pay plugins/autoclean plugins/fundchannel plugins/bcli plugins/esplora
```
- add libcurl to LDLIBS dep (needed for esplora plugin)
```
LDLIBS = -L/usr/local/lib -lm -lgmp -lsqlite3 -lz -lcurl $(COVFLAGS)
```
4. run make on your lightning folder

#### Run
Disable `bcli` plugin in order to fetch bitcoin data from `esplora` plugin, and set plugin options, as the following:
```
lightningd --testnet --disable-plugin bcli --log-level=debug \
 --esplora-api-endpoint=https://blockstream.info/testnet/api
```

Full available options:
- `--esplora-verbose=1`: enable curl verbosity
- `--esplora-api-endpoint=<url>`: set esplora endpoint (as https://blockstream.info/testnet/api for testnet)
- `--esplora-cainfo=<path>`: set path to Certificate Authority (CA) bundle (CA certificates extracted from Mozilla at https://curl.haxx.se/docs/caextract.html)
- `--esplora-capath=<path>`: specify directory holding CA certificates.
