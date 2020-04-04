# esplora_clnd_plugin
[c-lightning](https://github.com/ElementsProject/lightning) C plugin to use (as possible) [esplora web explorer](https://blockstream.info) to fetch bitcoin data.

1st c-lightning plugin with esplora integration is [sauron plugin](https://github.com/lightningd/plugins/tree/master/sauron) developped by [darosior](https://github.com/darosior).

#### Build

1. copy `esplora.c` and `Makefile` into `lightning/plugins` folder
2. edit main `lightning/Makefile` as the following:
- add esplora plugin to PLUGIN list
```
PLUGINS=plugins/pay plugins/autoclean plugins/fundchannel plugins/bcli plugins/esplora
```
- add libcurl to LDLIBS dep (needed for esplora plugin)
```
LDLIBS = -L/usr/local/lib -lm -lgmp -lsqlite3 -lz -lcurl $(COVFLAGS)
```
3. run make on your lightning folder

#### Run
Disable `bcli` plugin in order to fetch bitcoin data from `esplora` plugin, and set plugin options, as the following:
```
lightningd --testnet --disable-plugin bcli --log-level=debug \
--blockchair-api-endpoint https://api.blockchair.com/bitcoin/testnet --esplora-api-endpoint https://blockstream.info/testnet/api
```

Extra options:
- `--esplora-verbose=1`: enable curl verbosity
- `--esplora-cainfo=<path>`: set absolute ca info path for cacert.pem (CA certificates extracted from Mozilla at https://curl.haxx.se/docs/caextract.html)
- `--esplora-capath=<path>`: Specify directory holding CA certificates.
