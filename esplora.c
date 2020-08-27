#include <bitcoin/base58.h>
#include <bitcoin/block.h>
#include <bitcoin/feerate.h>
#include <bitcoin/script.h>
#include <bitcoin/shadouble.h>
#include <ccan/array_size/array_size.h>
#include <ccan/cast/cast.h>
#include <ccan/io/io.h>
#include <ccan/json_out/json_out.h>
#include <ccan/pipecmd/pipecmd.h>
#include <ccan/str/hex/hex.h>
#include <ccan/take/take.h>
#include <ccan/tal/grab_file/grab_file.h>
#include <ccan/tal/path/path.h>
#include <ccan/tal/str/str.h>
#include <common/json_helpers.h>
#include <common/memleak.h>
#include <common/utils.h>
#include <curl/curl.h>
#include <errno.h>
#include <inttypes.h>
#include <plugins/libplugin.h>

static char *endpoint = NULL;
static char *cainfo_path = NULL;
static char *capath = NULL;
static u64 verbose = 0;

struct curl_memory_data {
  u8 *memory;
  size_t size;
};

static bool get_u32_from_string(const tal_t *ctx, u32 *parsed_number,
                                const char *str, const char **err) {
  char *endp;
  u64 n;

  errno = 0;
  n = strtoul(str, &endp, 0);
  if (*endp || !str[0]) {
    *err = tal_fmt(NULL, "'%s' is not a number", str);
    return false;
  }
  if (errno) {
    *err = tal_fmt(NULL, "'%s' is out of range", str);
    return false;
  }

  *parsed_number = n;
  if (*parsed_number != n) {
    *err = tal_fmt(NULL, "'%s' is too large (overflow)", str);
    return false;
  }

  return true;
}

static size_t write_memory_callback(void *contents, size_t size, size_t nmemb,
                                    void *userp) {
  size_t realsize = size * nmemb;
  struct curl_memory_data *mem = (struct curl_memory_data *)userp;

  if (!tal_resize(&mem->memory, mem->size + realsize + 1)) {
    /* out of memory! */
    fprintf(stderr, "not enough memory (realloc returned NULL)\n");
    return 0;
  }

  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

static u8 *request(const tal_t *ctx, const char *url, const bool post,
                   const char *data) {
  struct curl_memory_data chunk;
  chunk.memory = tal_arr(ctx, u8, 64);
  chunk.size = 0;

  CURL *curl;
  CURLcode res;
  curl = curl_easy_init();
  if (!curl) {
    return NULL;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");
  if (verbose != 0)
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  if (cainfo_path != NULL)
    curl_easy_setopt(curl, CURLOPT_CAINFO, cainfo_path);
  if (capath != NULL)
    curl_easy_setopt(curl, CURLOPT_CAPATH, capath);
  if (post) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
  }
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);

  res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    return tal_free(chunk.memory);
  }
  long response_code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  if (response_code != 200) {
    return tal_free(chunk.memory);
  }
  curl_easy_cleanup(curl);
  tal_resize(&chunk.memory, chunk.size);
  return chunk.memory;
}

static char *request_get(const tal_t *ctx, const char *url) {
  return (char *)request(ctx, url, false, NULL);
}

static char *request_post(const tal_t *ctx, const char *url, const char *data) {
  return (char *)request(ctx, url, true, data);
}

static char *get_network_from_genesis_block(const char *blockhash) {
  if (strcmp(
          blockhash,
          "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f") ==
      0)
    return "main";
  else if (strcmp(blockhash, "000000000933ea01ad0ee984209779baaec3ced90fa3f4087"
                             "19526f8d77f4943") == 0)
    return "test";
  else if (strcmp(blockhash, "0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca5"
                             "90b1a11466e2206") == 0)
    return "regtest";
  else
    return NULL;
}

/* Get infos about the block chain.
 * Calls `getblockchaininfo` and returns headers count, blocks count,
 * the chain id, and whether this is initialblockdownload.
 */
static struct command_result *getchaininfo(struct command *cmd,
                                           const char *buf UNUSED,
                                           const jsmntok_t *toks UNUSED) {
  char *err;

  if (!param(cmd, buf, toks, NULL))
    return command_param_failed();

  plugin_log(cmd->plugin, LOG_INFORM, "getchaininfo");

  // fetch block genesis hash
  const char *block_genesis_url =
      tal_fmt(cmd->plugin, "%s/block-height/0", endpoint);
  const char *block_genesis = request_get(cmd, block_genesis_url);
  if (!block_genesis) {
    err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname,
                  block_genesis_url);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }
  plugin_log(cmd->plugin, LOG_INFORM, "block_genesis: %s", block_genesis);

  // fetch block count
  const char *blockcount_url =
      tal_fmt(cmd->plugin, "%s/blocks/tip/height", endpoint);
  const char *blockcount = request_get(cmd, blockcount_url);
  if (!blockcount) {
    err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname,
                  blockcount_url);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }
  plugin_log(cmd->plugin, LOG_INFORM, "blockcount: %s", blockcount);

  const char *error;
  u32 height;
  if (!get_u32_from_string(NULL, &height, blockcount, &error)) {
    err = tal_fmt(cmd, "%s: invalid height conversion on %s (error: %s)",
                  cmd->methodname, blockcount, error);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }

  // parsing blockgenesis to get the chain name information
  const char *chain = get_network_from_genesis_block(block_genesis);
  if (!chain) {
    err = tal_fmt(cmd, "%s: no chain found for genesis block %s",
                  cmd->methodname, block_genesis);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }

  // send response with chain information
  struct json_stream *response = jsonrpc_stream_success(cmd);
  json_add_string(response, "chain", chain);
  json_add_u32(response, "headercount", height);
  json_add_u32(response, "blockcount", height);
  json_add_bool(response, "ibd", false);

  return command_finished(cmd, response);
}

static struct command_result *
getrawblockbyheight_notfound(struct command *cmd) {
  struct json_stream *response;

  response = jsonrpc_stream_success(cmd);
  json_add_null(response, "blockhash");
  json_add_null(response, "block");

  return command_finished(cmd, response);
}

/* Get a raw block given its height.
 * Calls `getblockhash` then `getblock` to retrieve it from bitcoin_cli.
 * Will return early with null fields if block isn't known (yet).
 */
static struct command_result *getrawblockbyheight(struct command *cmd,
                                                  const char *buf,
                                                  const jsmntok_t *toks) {
  struct json_stream *response;
  u32 *height;
  char *err;

  if (!param(cmd, buf, toks, p_req("height", param_number, &height), NULL))
    return command_param_failed();

  plugin_log(cmd->plugin, LOG_INFORM, "getrawblockbyheight %d", *height);

  // fetch blockhash from block height
  const char *blockhash_url =
      tal_fmt(cmd->plugin, "%s/block-height/%d", endpoint, *height);
  const char *blockhash_ = request_get(cmd, blockhash_url);
  if (!blockhash_) {
    // block not found as getrawblockbyheight_notfound
    return getrawblockbyheight_notfound(cmd);
  }
  char *blockhash =
      tal_dup_arr(cmd, char, (char *)blockhash_, tal_count(blockhash_), 1);
  blockhash[tal_count(blockhash_)] = '\0';
  tal_free(blockhash_);
  plugin_log(cmd->plugin, LOG_INFORM, "blockhash: %s from %s", blockhash,
             blockhash_url);

  // Esplora serves raw block
  const char *block_url =
      tal_fmt(cmd->plugin, "%s/block/%s/raw", endpoint, blockhash);
  const u8 *block_res = request(cmd, block_url, false, NULL);
  if (!block_res) {
    err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname, block_url);
    plugin_log(cmd->plugin, LOG_INFORM, "%s", err);
    // block not found as getrawblockbyheight_notfound
    return getrawblockbyheight_notfound(cmd);
  }

  // parse rawblock output
  const char *rawblock =
      tal_hexstr(cmd->plugin, block_res, tal_count(block_res));
  if (!rawblock) {
    err = tal_fmt(cmd, "%s: convert error on %s", cmd->methodname, block_url);
    plugin_log(cmd->plugin, LOG_INFORM, "%s", err);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }

  // send response with block and blockhash in hex format
  response = jsonrpc_stream_success(cmd);
  json_add_string(response, "blockhash", blockhash);
  json_add_string(response, "block", rawblock);

  return command_finished(cmd, response);
}

/* Get current feerate.
 * Returns the feerate to lightningd as btc/k*VBYTE*.
 */
static struct command_result *estimatefees(struct command *cmd,
                                           const char *buf UNUSED,
                                           const jsmntok_t *toks UNUSED) {
  char *err;
  bool valid;
  // slow, normal, urgent, very_urgent
  int targets[4] = {144, 5, 3, 2};
  u64 feerates[4] = {1, 0, 0, 0};

  if (!param(cmd, buf, toks, NULL))
    return command_param_failed();

  // fetch feerates
  const char *feerate_url = tal_fmt(cmd->plugin, "%s/fee-estimates", endpoint);
  const char *feerate_res = request_get(cmd, feerate_url);
  if (!feerate_res) {
    err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname, feerate_url);
    plugin_log(cmd->plugin, LOG_INFORM, "err: %s", err);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }
  // parse feerates output
  const jsmntok_t *tokens =
      json_parse_input(cmd, feerate_res, strlen(feerate_res), &valid);
  if (!tokens) {
    err = tal_fmt(cmd, "%s: json error (%.*s)?", cmd->methodname,
                  (int)sizeof(feerate_res), feerate_res);
    plugin_log(cmd->plugin, LOG_INFORM, "err: %s", err);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }
  // Get the feerate for each target
  for (size_t i = 0; i < ARRAY_SIZE(feerates); i++) {
    const jsmntok_t *feeratetok = json_get_member(
        feerate_res, tokens, tal_fmt(cmd->plugin, "%d", targets[i]));
    // This puts a feerate in sat/vB multiplied by 10**7 in 'feerate' ...
    if (!feeratetok ||
        !json_to_millionths(feerate_res, feeratetok, &feerates[i])) {
      err = tal_fmt(cmd, "%s: had no feerate for block %d (%.*s)?",
                    cmd->methodname, targets[i], (int)sizeof(feerate_res),
                    feerate_res);
      plugin_log(cmd->plugin, LOG_INFORM, "err: %s", err);
      if (i > 0)
        feerates[i] = feerates[i - 1];
    }

    // ... But lightningd wants a sat/kVB feerate, divide by 10**4 !
    feerates[i] /= 10000;
  }

  struct json_stream *response = jsonrpc_stream_success(cmd);
  json_add_u64(response, "opening", feerates[1]);
  json_add_u64(response, "mutual_close", feerates[1]);
  json_add_u64(response, "unilateral_close", feerates[3]);
  json_add_u64(response, "delayed_to_us", feerates[1]);
  json_add_u64(response, "htlc_resolution", feerates[2]);
  json_add_u64(response, "penalty", feerates[2]);
  /* We divide the slow feerate for the minimum acceptable, lightningd
   * will use floor if it's hit, though. */
  json_add_u64(response, "min_acceptable", feerates[0] / 2);
  /* BOLT #2:
   *
   * Given the variance in fees, and the fact that the transaction may be
   * spent in the future, it's a good idea for the fee payer to keep a good
   * margin (say 5x the expected fee requirement)
   *
   * 10 is lightningd's default for bitcoind-max-multiplier
   */
  json_add_u64(response, "max_acceptable", feerates[3] * 10);

  return command_finished(cmd, response);
}

static struct command_result *getutxout(struct command *cmd, const char *buf,
                                        const jsmntok_t *toks) {
  struct json_stream *response;
  const char *txid, *vout;
  char *err;
  bool spent = false;
  jsmntok_t *tokens;
  struct bitcoin_tx_output output;
  bool valid = false;

  plugin_log(cmd->plugin, LOG_INFORM, "getutxout");

  /* bitcoin-cli wants strings. */
  if (!param(cmd, buf, toks, p_req("txid", param_string, &txid),
             p_req("vout", param_string, &vout), NULL))
    return command_param_failed();

  // convert vout to number
  const char *error;
  u32 vout_index;
  if (!get_u32_from_string(cmd, &vout_index, vout, &error)) {
    const char *err = tal_fmt(
        cmd, "Conversion error occurred on %s (error: %s)", vout, error);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }

  // check transaction output is spent
  const char *status_url =
      tal_fmt(cmd->plugin, "%s/tx/%s/outspend/%s", endpoint, txid, vout);
  const char *status_res = request_get(cmd, status_url);
  if (!status_res) {
    err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname, status_url);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }
  tokens = json_parse_input(cmd, status_res, strlen(status_res), &valid);
  if (!tokens || !valid) {
    err = tal_fmt(cmd, "%s: json error (%.*s)?", cmd->methodname,
                  (int)sizeof(status_res), status_res);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }

  // parsing spent field
  const jsmntok_t *spenttok = json_get_member(status_res, tokens, "spent");
  if (!spenttok || !json_to_bool(status_res, spenttok, &spent)) {
    err = tal_fmt(cmd, "%s: had no spent (%.*s)?", cmd->methodname,
                  (int)sizeof(status_res), status_res);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }
  /* As of at least v0.15.1.0, bitcoind returns "success" but an empty
     string on a spent txout. */
  if (spent) {
    response = jsonrpc_stream_success(cmd);
    json_add_null(response, "amount");
    json_add_null(response, "script");
    return command_finished(cmd, response);
  }

  // get transaction information
  const char *gettx_url = tal_fmt(cmd->plugin, "%s/tx/%s", endpoint, txid);
  const char *gettx_res = request_get(cmd, gettx_url);
  if (!gettx_res) {
    err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname, gettx_url);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }
  tokens = json_parse_input(cmd, gettx_res, strlen(gettx_res), &valid);
  if (!tokens || !valid) {
    err = tal_fmt(cmd, "%s: json error (%.*s)?", cmd->methodname,
                  (int)sizeof(gettx_res), gettx_res);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }

  // parsing vout array field
  const jsmntok_t *vouttok = json_get_member(gettx_res, tokens, "vout");
  if (!vouttok) {
    err = tal_fmt(cmd, "%s: had no vout (%.*s)?", cmd->methodname,
                  (int)sizeof(gettx_res), gettx_res);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }
  const jsmntok_t *v = json_get_arr(vouttok, vout_index);
  if (!v) {
    err = tal_fmt(cmd, "%s: had no vout[%d] (%.*s)?", cmd->methodname,
                  (int)vout_index, (int)sizeof(gettx_res), gettx_res);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }

  // parsing amount value
  const jsmntok_t *valuetok = json_get_member(gettx_res, v, "value");
  if (!valuetok ||
      !json_to_bitcoin_amount(
          gettx_res, valuetok,
          &output.amount.satoshis)) { /* Raw: talking to bitcoind */
    err = tal_fmt(cmd, "%s: had no vout[%d] value (%.*s)?", cmd->methodname,
                  vout_index, (int)sizeof(gettx_res), gettx_res);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }

  // parsing scriptpubkey
  const jsmntok_t *scriptpubkeytok =
      json_get_member(gettx_res, v, "scriptpubkey");
  if (!scriptpubkeytok) {
    err =
        tal_fmt(cmd, "%s: had no vout[%d] scriptpubkey (%.*s)?",
                cmd->methodname, vout_index, (int)sizeof(gettx_res), gettx_res);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }
  output.script = tal_hexdata(cmd, gettx_res + scriptpubkeytok->start,
                              scriptpubkeytok->end - scriptpubkeytok->start);
  if (!output.script) {
    err = tal_fmt(cmd, "%s: scriptpubkey invalid hex (%.*s)?", cmd->methodname,
                  (int)sizeof(gettx_res), gettx_res);
    return command_done_err(cmd, BCLI_ERROR, err, NULL);
  }

  // replay response
  response = jsonrpc_stream_success(cmd);
  json_add_amount_sat_only(response, "amount", output.amount);
  json_add_string(response, "script",
                  tal_hexstr(response, output.script, sizeof(output.script)));

  return command_finished(cmd, response);
}

/* Send a transaction to the Bitcoin network.
 * Calls `sendrawtransaction` using the first parameter as the raw tx.
 */
static struct command_result *sendrawtransaction(struct command *cmd,
                                                 const char *buf,
                                                 const jsmntok_t *toks) {
  const char *tx;

  /* bitcoin-cli wants strings. */
  if (!param(cmd, buf, toks, p_req("tx", param_string, &tx), NULL))
    return command_param_failed();

  plugin_log(cmd->plugin, LOG_INFORM, "sendrawtransaction");

  // request post passing rawtransaction
  const char *sendrawtx_url = tal_fmt(cmd->plugin, "%s/tx", endpoint);
  const char *res = request_post(cmd, sendrawtx_url, tx);
  struct json_stream *response = jsonrpc_stream_success(cmd);
  if (!res) {
    // send response with failure
    const char *err =
        tal_fmt(cmd, "%s: invalid tx (%.*s)? on (%.*s)?", cmd->methodname,
                (int)sizeof(tx), tx, (int)sizeof(sendrawtx_url), sendrawtx_url);
    json_add_bool(response, "success", false);
    json_add_string(response, "errmsg", err);
  }

  // send response with success
  json_add_bool(response, "success", true);
  json_add_string(response, "errmsg", "");
  return command_finished(cmd, response);
}

static void init(struct plugin *p, const char *buffer UNUSED,
                 const jsmntok_t *config UNUSED) {
  plugin_log(p, LOG_INFORM, "esplora initialized.");
}

static const struct plugin_command commands[] = {
    {"getrawblockbyheight", "bitcoin",
     "Get the bitcoin block at a given height", "", getrawblockbyheight},
    {"getchaininfo", "bitcoin",
     "Get the chain id, the header count, the block count,"
     " and whether this is IBD.",
     "", getchaininfo},
    {"estimatefees", "bitcoin", "Get the Bitcoin feerate in btc/kilo-vbyte.",
     "", estimatefees},
    {"sendrawtransaction", "bitcoin",
     "Send a raw transaction to the Bitcoin network.", "", sendrawtransaction},
    {"getutxout", "bitcoin",
     "Get informations about an output, identified by a {txid} an a {vout}", "",
     getutxout},
};

int main(int argc, char *argv[]) {
  setup_locale();

  plugin_main(argv, init, PLUGIN_STATIC, false, NULL, commands,
              ARRAY_SIZE(commands), NULL, 0, NULL, 0,
              plugin_option(
                  "esplora-api-endpoint", "string",
                  "The URL of the esplora instance to hit (including '/api').",
                  charp_option, &endpoint),
              plugin_option("esplora-cainfo", "string",
                            "Set path to Certificate Authority (CA) bundle.",
                            charp_option, &cainfo_path),
              plugin_option("esplora-capath", "string",
                            "Specify directory holding CA certificates.",
                            charp_option, &capath),
              plugin_option("esplora-verbose", "int",
                            "Set verbose output (default 0).", u64_option,
                            &verbose),
              NULL);
}
