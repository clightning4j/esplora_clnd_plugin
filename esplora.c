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
#include <stdlib.h>

/* Esplora base URL */
const char *BASE_URL = "https://blockstream.info";
const char *BASE_URL_TORV2 = "http://explorernuoc63nb.onion";
const char *BASE_URL_TORV3 =
    "http://explorerzydxu5ecjrkwceayqybizmpjjznk5izmitf2modhcusuqlid.onion";

struct proxy_conf {
	/* Simple flag to check if the proxy is enabled by configuration*/
	bool proxy_enabled;

	/* Proxy address, e.g: 127.0.0.1 */
	char *address;

	/* Proxy port, e.g: 9050 */
	unsigned int port;

	/* Tor v3 enabled */
	bool torv3_enabled;

	/* lightnind require that the proxy is enabled always */
	bool always_used;
};

struct esplora {

	/* The endpoint to query for Bitcoin data. */
	char *endpoint;

	/* CA stuff for TLS. */
	char *cainfo_path;
	char *capath;

	/* Make curl request more verbose. */
	bool verbose;

	/* Make curl request over proxy socks5 */
	bool proxy_disabled;

	/* How many times do we retry curl requests ? */
	u32 n_retries;
};

static struct esplora *esplora;
static struct proxy_conf *proxy_conf;

struct curl_memory_data {
	u8 *memory;
	size_t size;
};

static bool get_u32_from_string(const tal_t *ctx, u32 *parsed_number,
				const char *str, const char **err)
{
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
				    void *userp)
{
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

/** Preform a curl request, retrying up to `n_retries` times. */
static bool perform_request(CURL *curl)
{
	CURLcode res;
	u32 retries = 0;

	for (;;) {
		res = curl_easy_perform(curl);
		if (res == CURLE_OK)
			return true;

		if (++retries > esplora->n_retries)
			return false;
		sleep(1);
	}
}

static u8 *request(const tal_t *ctx, const char *url, const bool post,
		   const char *data)
{
	long response_code;
	struct curl_memory_data chunk;
	chunk.memory = tal_arr(ctx, u8, 64);
	chunk.size = 0;

	CURL *curl;
	curl = curl_easy_init();
	if (!curl) {
		return NULL;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");
	if (!esplora->proxy_disabled && proxy_conf->proxy_enabled) {
		int length = snprintf(NULL, 0, "%d", proxy_conf->port);
		// This contains +2 because I added the separator : before to
		// add port number!
		char *str = malloc(length + 2);
		snprintf(str, length + 2, ":%d", proxy_conf->port);
		char *address = tal_strcat(ctx, proxy_conf->address, str);
		char *curl_query = tal_strcat(ctx, "socks5h://", address);
		curl_easy_setopt(curl, CURLOPT_PROXY, curl_query);
	}
	if (esplora->verbose)
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
	if (esplora->cainfo_path != NULL)
		curl_easy_setopt(curl, CURLOPT_CAINFO, esplora->cainfo_path);
	if (esplora->capath != NULL)
		curl_easy_setopt(curl, CURLOPT_CAPATH, esplora->capath);
	if (post) {
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
	}
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);

	/* This populates the curl struct on success. */
	if (!perform_request(curl))
		return tal_free(chunk.memory);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
	if (response_code != 200)
		return tal_free(chunk.memory);

	curl_easy_cleanup(curl);
	tal_resize(&chunk.memory, chunk.size);

	return chunk.memory;
}

static char *request_get(const tal_t *ctx, const char *url)
{
	return (char *)request(ctx, url, false, NULL);
}

static char *request_post(const tal_t *ctx, const char *url, const char *data)
{
	return (char *)request(ctx, url, true, data);
}

static char *get_network_from_genesis_block(const char *blockhash)
{
	if (strncmp(blockhash,
		    "000000000019d6689c085ae165831e934ff763ae46a2a6c1"
		    "72b3f1b60a8ce26f",
		    64) == 0)
		return "main";
	else if (strncmp(blockhash,
			 "000000000933ea01ad0ee984209779baaec3ced90fa3f4087"
			 "19526f8d77f4943",
			 64) == 0)
		return "test";
	else if (strncmp(blockhash,
			 "1466275836220db2944ca059a3a10ef6fd2ea684b0688d2c3"
			 "79296888a206003",
			 64) == 0)
		return "liquidv1";
	else if (strncmp(blockhash,
			 "0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca5"
			 "90b1a11466e2206",
			 64) == 0)
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
					   const jsmntok_t *toks UNUSED)
{
	char *err;

	if (!param(cmd, buf, toks, NULL))
		return command_param_failed();

	plugin_log(cmd->plugin, LOG_INFORM, "getchaininfo");

	// fetch block genesis hash
	const char *block_genesis_url =
	    tal_fmt(cmd->plugin, "%s/block-height/0", esplora->endpoint);
	const char *block_genesis = request_get(cmd, block_genesis_url);
	if (!block_genesis) {
		err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname,
			      block_genesis_url);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}
	plugin_log(cmd->plugin, LOG_INFORM, "block_genesis: %s", block_genesis);

	// fetch block count
	const char *blockcount_url =
	    tal_fmt(cmd->plugin, "%s/blocks/tip/height", esplora->endpoint);
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
		err = tal_fmt(cmd,
			      "%s: invalid height conversion on %s (error: %s)",
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

static struct command_result *getrawblockbyheight_notfound(struct command *cmd)
{
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
static struct command_result *
getrawblockbyheight(struct command *cmd, const char *buf, const jsmntok_t *toks)
{
	struct json_stream *response;
	u32 *height;
	char *err;

	if (!param(cmd, buf, toks, p_req("height", param_number, &height),
		   NULL))
		return command_param_failed();

	plugin_log(cmd->plugin, LOG_INFORM, "getrawblockbyheight %d", *height);

	// fetch blockhash from block height
	const char *blockhash_url = tal_fmt(cmd->plugin, "%s/block-height/%d",
					    esplora->endpoint, *height);
	const char *blockhash_ = request_get(cmd, blockhash_url);
	if (!blockhash_) {
		// block not found as getrawblockbyheight_notfound
		return getrawblockbyheight_notfound(cmd);
	}
	char *blockhash = tal_dup_arr(cmd, char, (char *)blockhash_,
				      tal_count(blockhash_), 1);
	blockhash[tal_count(blockhash_)] = '\0';
	tal_free(blockhash_);
	plugin_log(cmd->plugin, LOG_INFORM, "blockhash: %s from %s", blockhash,
		   blockhash_url);

	// Esplora serves raw block
	const char *block_url = tal_fmt(cmd->plugin, "%s/block/%s/raw",
					esplora->endpoint, blockhash);
	const u8 *block_res = request(cmd, block_url, false, NULL);
	if (!block_res) {
		err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname,
			      block_url);
		plugin_log(cmd->plugin, LOG_INFORM, "%s", err);
		// block not found as getrawblockbyheight_notfound
		return getrawblockbyheight_notfound(cmd);
	}

	// parse rawblock output
	const char *rawblock =
	    tal_hexstr(cmd->plugin, block_res, tal_count(block_res));
	if (!rawblock) {
		err = tal_fmt(cmd, "%s: convert error on %s", cmd->methodname,
			      block_url);
		plugin_log(cmd->plugin, LOG_INFORM, "%s", err);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}

	// send response with block and blockhash in hex format
	response = jsonrpc_stream_success(cmd);
	json_add_string(response, "blockhash", blockhash);
	json_add_string(response, "block", rawblock);

	return command_finished(cmd, response);
}

static struct command_result *estimatefees_null_response(struct command *cmd)
{
	struct json_stream *response = jsonrpc_stream_success(cmd);

	json_add_null(response, "opening");
	json_add_null(response, "mutual_close");
	json_add_null(response, "unilateral_close");
	json_add_null(response, "delayed_to_us");
	json_add_null(response, "htlc_resolution");
	json_add_null(response, "penalty");
	json_add_null(response, "min_acceptable");
	json_add_null(response, "max_acceptable");

	return command_finished(cmd, response);
}

/* Get current feerate.
 * Returns the feerate to lightningd as btc/k*VBYTE*.
 */
static struct command_result *estimatefees(struct command *cmd,
					   const char *buf UNUSED,
					   const jsmntok_t *toks UNUSED)
{
	char *err;
	// slow, normal, urgent, very_urgent
	int targets[4] = {144, 5, 3, 2};
	u64 *feerates = tal_arr(NULL, u64, 4);

	if (!param(cmd, buf, toks, NULL))
		return command_param_failed();

	const char *feerate_url =
	    // fetch feerates
	    tal_fmt(cmd->plugin, "%s/fee-estimates", esplora->endpoint);
	const char *feerate_res = request_get(cmd, feerate_url);
	if (!feerate_res) {
		err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname,
			      feerate_url);
		plugin_log(cmd->plugin, LOG_UNUSUAL, "err: %s", err);
		return estimatefees_null_response(cmd);
	}
	// parse feerates output
	const jsmntok_t *tokens =
	    json_parse_simple(cmd, feerate_res, strlen(feerate_res));
	if (!tokens) {
		err = tal_fmt(cmd, "%s: json error (%.*s)?", cmd->methodname,
			      (int)sizeof(feerate_res), feerate_res);
		plugin_log(cmd->plugin, LOG_INFORM, "err: %s", err);
		return estimatefees_null_response(cmd);
	}
	for (size_t i = 0; i < tal_count(feerates); i++) {
		const jsmntok_t *feeratetok =
		    json_get_member(feerate_res, tokens,
				    tal_fmt(cmd->plugin, "%d", targets[i]));
		// This puts a feerate in sat/vB multiplied by 10**7 in
		// 'feerate'.
		// Esplora can answer with a empty object like this {}, in this
		// case we need to return a null response to say that is not
		// possible to estimate the feerate.
		if (!feeratetok || !json_to_millionths(feerate_res, feeratetok,
						       &feerates[i])) {
			err = tal_fmt(cmd,
				      "%s: had no feerate for block %d (%.*s)?",
				      cmd->methodname, targets[i],
				      (int)sizeof(feerate_res), feerate_res);
			plugin_log(cmd->plugin, LOG_INFORM, "err: %s", err);
			return estimatefees_null_response(cmd);
		}

		// ... But lightningd wants a sat/kVB feerate, divide by 10**4 !
		feerates[i] /= 10000;
	}
	// sanity check
	if (!feerates)
		return estimatefees_null_response(cmd);

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
	 * spent in the future, it's a good idea for the fee payer to keep a
	 * good margin (say 5x the expected fee requirement)
	 *
	 * 10 is lightningd's default for bitcoind-max-multiplier
	 */
	json_add_u64(response, "max_acceptable", feerates[3] * 10);

	return command_finished(cmd, response);
}

static struct command_result *getutxout(struct command *cmd, const char *buf,
					const jsmntok_t *toks)
{
	struct json_stream *response;
	const char *txid, *vout;
	char *err;
	bool spent = false;
	jsmntok_t *tokens;
	struct bitcoin_tx_output output;

	plugin_log(cmd->plugin, LOG_INFORM, "getutxout");

	/* bitcoin-cli wants strings. */
	if (!param(cmd, buf, toks, p_req("txid", param_string, &txid),
		   p_req("vout", param_string, &vout), NULL))
		return command_param_failed();

	// convert vout to number
	const char *error;
	u32 vout_index;
	if (!get_u32_from_string(cmd, &vout_index, vout, &error)) {
		const char *err =
		    tal_fmt(cmd, "Conversion error occurred on %s (error: %s)",
			    vout, error);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}

	// check transaction output is spent
	const char *status_url = tal_fmt(cmd->plugin, "%s/tx/%s/outspend/%s",
					 esplora->endpoint, txid, vout);
	const char *status_res = request_get(cmd, status_url);
	if (!status_res) {
		err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname,
			      status_url);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}
	tokens = json_parse_simple(cmd, status_res, strlen(status_res));
	if (!tokens) {
		err = tal_fmt(cmd, "%s: json error (%.*s)?", cmd->methodname,
			      (int)sizeof(status_res), status_res);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}

	// parsing spent field
	const jsmntok_t *spenttok =
	    json_get_member(status_res, tokens, "spent");
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
	const char *gettx_url =
	    tal_fmt(cmd->plugin, "%s/tx/%s", esplora->endpoint, txid);
	const char *gettx_res = request_get(cmd, gettx_url);
	if (!gettx_res) {
		err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname,
			      gettx_url);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}
	tokens = json_parse_simple(cmd, gettx_res, strlen(gettx_res));
	if (!tokens) {
		err = tal_fmt(cmd, "%s: json error (%.*s)?", cmd->methodname,
			      (int)sizeof(gettx_res), gettx_res);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}

	const char *guide =
	    tal_fmt(cmd->plugin, "{vout:[%d:{value:%%,scriptpubkey:%%}]}",
		    (int)vout_index);
	error = json_scan(
	    cmd, gettx_res, tokens, guide,
	    JSON_SCAN(json_to_sat, &output.amount),
	    JSON_SCAN_TAL(cmd, json_tok_bin_from_hex, &output.script));
	if (error)
		return command_done_err(cmd, BCLI_ERROR, error, NULL);

	// replay response
	response = jsonrpc_stream_success(cmd);
	json_add_amount_sat_only(response, "amount", output.amount);
	json_add_string(response, "script", tal_hex(response, output.script));

	return command_finished(cmd, response);
}

/* Send a transaction to the Bitcoin network.
 * Calls `sendrawtransaction` using the first parameter as the raw tx.
 */
static struct command_result *
sendrawtransaction(struct command *cmd, const char *buf, const jsmntok_t *toks)
{
	const char *tx;
	// FIXME(vincenzopalazzo) This propriety is added in the version 0.9.1
	// We can try to give a meaning to it. For the moment
	// it is only a fix to work around the param method error
	bool *allowhighfees;
	/* bitcoin-cli wants strings. */
	if (!param(
		cmd, buf, toks, p_req("tx", param_string, &tx),
		p_opt_def("allowhighfees", param_bool, &allowhighfees, false),
		NULL))
		return command_param_failed();

	plugin_log(cmd->plugin, LOG_INFORM, "sendrawtransaction");

	// request post passing rawtransaction
	const char *sendrawtx_url =
	    tal_fmt(cmd->plugin, "%s/tx", esplora->endpoint);
	const char *res = request_post(cmd, sendrawtx_url, tx);
	struct json_stream *response = jsonrpc_stream_success(cmd);
	if (!res) {
		// send response with failure
		const char *err =
		    tal_fmt(cmd, "%s: invalid tx (%.*s)? on (%.*s)?",
			    cmd->methodname, (int)sizeof(tx), tx,
			    (int)sizeof(sendrawtx_url), sendrawtx_url);
		json_add_bool(response, "success", false);
		json_add_string(response, "errmsg", err);
		return command_finished(cmd, response);
	}

	// send response with success
	json_add_bool(response, "success", true);
	json_add_string(response, "errmsg", "");
	return command_finished(cmd, response);
}

static void configure_url(const char *network, bool proxy_enabled,
			  bool torv3_enabled)
{
	if (proxy_enabled && !esplora->proxy_disabled) {
		if (torv3_enabled)
			esplora->endpoint =
			    tal_strcat(NULL, BASE_URL_TORV3, network);
		else
			esplora->endpoint =
			    tal_strcat(NULL, BASE_URL_TORV2, network);
	} else {
		esplora->endpoint = tal_strcat(NULL, BASE_URL, network);
	}
}

static bool configure_esplora_with_network(const char *network,
					   bool proxy_enabled,
					   bool torv3_enabled)
{
	// FIXME(vincenzopalazzo) In this case if the endpoint is not null
	// inside the URL, we can try to see the format of URL insert inside
	// the command line if mach with some format that the node aspect.
	// e.g: If the plugin receive an URL without tor for, is a good manner
	// throws an log message (UNUSUAL) with some information about the
	// conflics.
	if (esplora->endpoint != NULL)
		return true;
	if (streq(network, "testnet")) {
		configure_url("/testnet/api", proxy_enabled, torv3_enabled);
		return true;
	} else if (streq(network, "bitcoin")) {
		configure_url("/api", proxy_enabled, torv3_enabled);
		return true;
	} else if (streq(network, "liquid")) {
		configure_url("/liquid/api", proxy_enabled, torv3_enabled);
		return true;
	}
	// Unsupported network!
	return false;
}

static const char *init(struct plugin *p, const char *buffer,
			const jsmntok_t *config)
{
	const jsmntok_t *proxy_tok = json_get_member(buffer, config, "proxy");
	if (proxy_tok) {
		const jsmntok_t *address_tok =
		    json_get_member(buffer, proxy_tok, "address");
		const jsmntok_t *port_tok =
		    json_get_member(buffer, proxy_tok, "port");
		const jsmntok_t *torv3_tok =
		    json_get_member(buffer, config, "torv3-enabled");
		const jsmntok_t *always_proxy =
		    json_get_member(buffer, config, "use_proxy_always");
		if (address_tok && port_tok && torv3_tok && always_proxy) {
			proxy_conf->proxy_enabled = true;
			proxy_conf->address =
			    json_strdup(NULL, buffer, address_tok);
			json_to_number(buffer, port_tok, &proxy_conf->port);
			json_to_bool(buffer, torv3_tok,
				     &proxy_conf->torv3_enabled);
			json_to_bool(buffer, always_proxy,
				     &proxy_conf->always_used);
		}
	}

	const jsmntok_t *network_tok =
	    json_get_member(buffer, config, "network");

	char *network = json_strdup(NULL, buffer, network_tok);
	if (!configure_esplora_with_network(network, proxy_conf->proxy_enabled,
					    proxy_conf->torv3_enabled))
		plugin_log(p, LOG_UNUSUAL, "Network %s unsupported", network);

	// Is good manners for the moment maintains this check only a warning
	// and not abort if the config is uncorrect, we are inside the
	// developing stage in some cases we need to disable the proxy inside
	// the plugin to make test in debugging stage.
	if (proxy_conf->always_used && !esplora->proxy_disabled)
		plugin_log(
		    p, LOG_UNUSUAL,
		    "lightnind require the proxy always,"
		    "in this cases the esplora plugin should be use the proxy");

	plugin_log(p, LOG_INFORM,
		   "------------ esplora initialized ------------");
	plugin_log(p, LOG_INFORM, "esplora endpoint %s", esplora->endpoint);
	if (proxy_conf->proxy_enabled && !esplora->proxy_disabled)
		plugin_log(p, LOG_INFORM, "proxy configuration %s:%d",
			   proxy_conf->address, proxy_conf->port);
	return NULL;
}

static struct esplora *new_esplora(const tal_t *ctx)
{
	struct esplora *esplora = tal(ctx, struct esplora);

	esplora->endpoint = NULL;
	esplora->capath = NULL;
	esplora->cainfo_path = NULL;
	esplora->verbose = false;
	esplora->proxy_disabled = false;
	esplora->n_retries = 4;

	return esplora;
}

static struct proxy_conf *new_proxy_conf(const tal_t *ctx)
{
	struct proxy_conf *proxy_conf = tal(ctx, struct proxy_conf);

	proxy_conf->proxy_enabled = false;
	proxy_conf->address = NULL;
	proxy_conf->port = 9050;
	proxy_conf->torv3_enabled = false;
	proxy_conf->always_used = false;

	return proxy_conf;
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
     "Get information about an output, identified by a {txid} an a {vout}", "",
     getutxout},
};

int main(int argc, char *argv[])
{
	setup_locale();

	/* Our global state. */
	esplora = new_esplora(NULL);
	proxy_conf = new_proxy_conf(NULL);

	plugin_main(
	    argv, init, PLUGIN_STATIC, false, NULL, commands,
	    ARRAY_SIZE(commands), NULL, 0, NULL, 0,
	    plugin_option("esplora-api-endpoint", "string",
			  "The URL of the esplora instance to hit "
			  "(including '/api').",
			  charp_option, &esplora->endpoint),
	    plugin_option("esplora-cainfo", "string",
			  "Set path to Certificate Authority (CA) bundle.",
			  charp_option, &esplora->cainfo_path),
	    plugin_option("esplora-capath", "string",
			  "Specify directory holding CA certificates.",
			  charp_option, &esplora->capath),
	    plugin_option("esplora-verbose", "bool",
			  "Set verbose output (default: false).", bool_option,
			  &esplora->verbose),
	    plugin_option("esplora-retries", "string",
			  "How many times should we retry a request to the"
			  "endpoint before dying ?",
			  u32_option, &esplora->n_retries),
	    plugin_option("esplora-disable-proxy", "flag",
			  "Ignore the proxy setting inside lightningd conf.",
			  flag_option, &esplora->proxy_disabled),
	    NULL);
}
