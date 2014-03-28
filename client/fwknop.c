/**
 * @file    fwknop.c
 *
 * @brief   The fwknop client.
 *
 *  Fwknop is developed primarily by the people listed in the file 'AUTHORS'.
 *  Copyright (C) 2009-2014 fwknop developers and contributors. For a full
 *  list of contributors, see the file 'CREDITS'.
 *
 *  License (GNU General Public License):
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *  USA
 */

#include "fwknop.h"
#include "config_init.h"
#include "spa_comm.h"
#include "utils.h"
#include "getpasswd.h"

#include <sys/stat.h>
#include <fcntl.h>


/* prototypes
*/
static int get_keys(fko_ctx_t ctx, fko_cli_options_t *options,
    char *key, int *key_len, char *hmac_key, int *hmac_key_len);
static void errmsg(const char *msg, const int err);
static int prev_exec(fko_cli_options_t *options, int argc, char **argv);
static int get_save_file(char *args_save_file);
static int show_last_command(const char * const args_save_file);
static int save_args(int argc, char **argv, const char * const args_save_file);
static int run_last_args(fko_cli_options_t *options,
        const char * const args_save_file);
static int set_message_type(fko_ctx_t ctx, fko_cli_options_t *options);
static int set_nat_access(fko_ctx_t ctx, fko_cli_options_t *options,
        const char * const access_buf);
static int set_access_buf(fko_ctx_t ctx, fko_cli_options_t *options,
        char *access_buf);
static int get_rand_port(void);
int resolve_ip_http(fko_cli_options_t *options);
static void clean_exit(fko_ctx_t ctx, fko_cli_options_t *opts,
    char *key, int *key_len, char *hmac_key, int *hmac_key_len,
    unsigned int exit_status);
static void zero_buf_wrapper(char *buf, int len);
static int is_hostname_str_with_port(const char *str,
        char *hostname, size_t hostname_bufsize, int *port);

#define MAX_CMDLINE_ARGS            50                  /*!< should be way more than enough */
#define NAT_ACCESS_STR_TEMPLATE     "%s,%d"             /*!< Template for a nat access string ip,port with sscanf*/
#define HOSTNAME_BUFSIZE            64                  /*!< Maximum size of a hostname string */
#define CTX_DUMP_BUFSIZE            4096                /*!< Maximum size allocated to a FKO context dump */

/**
 * @brief Check whether a string is an ipv6 address or not
 *
 * @param str String to check for an ipv6 address.
 *
 * @return 1 if the string is an ipv6 address, 0 otherwise.
 */
static int
is_ipv6_str(char *str)
{
    return 0;
}

/**
 * @brief Check a string to find out if it is built as 'hostname,port'
 *
 * This function check if we can extract an hostname and a port from the string.
 * If yes, we return 1, and both the hostname buffer and the port number are set
 * accordingly.
 *
 * We could have used sscanf() here with a template "%[^,],%u", but this way we
 * do not limit the size of the value copy in the hostname destination buffer.
 * Limiting the string in the sscanf() can be done but would prevent any easy change
 * for the hostname buffer size.
 *
 * @param str String to parse.
 * @param hostname Buffer where to store the hostname value read from @str.
 * @param hostname_bufsize Hostname buffer size.
 * @param port Value of the port read from @str.
 *
 * @return 1 if the string is built as 'hostname,port', 0 otherwise.
 */
static int
is_hostname_str_with_port(const char *str, char *hostname, size_t hostname_bufsize, int *port)
{
    int     valid = 0;                /* Result of the function */
    char    buf[MAX_LINE_LEN] = {0};  /* Copy of the buffer eg. "hostname,port" */
    char   *h;                        /* Pointer on the hostname string */
    char   *p;                        /* Ponter on the port string */

    memset(hostname, 0, hostname_bufsize);
    *port = 0;

    /* Replace the comma in the string with a NULL char to split the
     * buffer in two strings (hostname and port) */
    strlcpy(buf, str, sizeof(buf));
    p = strchr(buf, ',');

    if(p != NULL)
    {
        *p++ = 0;
        h = buf;

        *port = atoi(p);

        /* If the string does not match an ipv4 or ipv6 address we assume this
         * is an hostname. We make sure the port is in the good range too */
        if (   (is_valid_ipv4_addr(buf) == 0)
            && (is_ipv6_str(buf) == 0)
            && ((*port > 0) && (*port < 65536)) )
        {
            strlcpy(hostname, h, hostname_bufsize);
            valid = 1;
        }

        /* The port is out of range or the ip is an ipv6 or ipv4 address */
        else;
    }

    /* No port found in the string, let's skip */
    else;

    return valid;
}

int
main(int argc, char **argv)
{
    fko_ctx_t           ctx  = NULL;
    fko_ctx_t           ctx2 = NULL;
    int                 res;
    char               *spa_data=NULL, *version=NULL;
    char                access_buf[MAX_LINE_LEN] = {0};
    char                key[MAX_KEY_LEN+1]       = {0};
    char                hmac_key[MAX_KEY_LEN+1]  = {0};
    int                 key_len = 0, orig_key_len = 0, hmac_key_len = 0;
    int                 enc_mode = FKO_DEFAULT_ENC_MODE;
    int                 rand_mode = FKO_DEFAULT_RAND_MODE;
    int                 tmp_port = 0;
    char                dump_buf[CTX_DUMP_BUFSIZE];

    fko_cli_options_t   options;

    memset(&options, 0x0, sizeof(fko_cli_options_t));

    /* Initialize the log module */
    log_new();

    /* Handle command line
    */
    config_init(&options, argc, argv);

    /* Handle previous execution arguments if required
    */
    if(prev_exec(&options, argc, argv) != 1)
        clean_exit(ctx, &options, key, &key_len, hmac_key,
                &hmac_key_len, EXIT_FAILURE);

    if(options.show_last_command)
        clean_exit(ctx, &options, key, &key_len, hmac_key,
                &hmac_key_len, EXIT_SUCCESS);

    /* Intialize the context
    */
    res = fko_new(&ctx);
    if(res != FKO_SUCCESS)
    {
        errmsg("fko_new", res);
        clean_exit(ctx, &options, key, &key_len, hmac_key,
                &hmac_key_len, EXIT_FAILURE);
    }

    /* Display version info and exit.
    */
    if(options.version)
    {
        fko_get_version(ctx, &version);

        fprintf(stdout, "fwknop client %s, FKO protocol version %s\n",
            MY_VERSION, version);

        clean_exit(ctx, &options, key, &key_len,
            hmac_key, &hmac_key_len, EXIT_SUCCESS);
    }

    /* Set client timeout
    */
    if(options.fw_timeout >= 0)
    {
        res = fko_set_spa_client_timeout(ctx, options.fw_timeout);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_spa_client_timeout", res);
            clean_exit(ctx, &options, key, &key_len,
                hmac_key, &hmac_key_len, EXIT_FAILURE);
        }
    }

    /* Set the SPA packet message type based on command line options
    */
    res = set_message_type(ctx, &options);
    if(res != FKO_SUCCESS)
    {
        errmsg("fko_set_spa_message_type", res);
        clean_exit(ctx, &options, key, &key_len,
            hmac_key, &hmac_key_len, EXIT_FAILURE);
    }

    /* Adjust the SPA timestamp if necessary
    */
    if(options.time_offset_plus > 0)
    {
        res = fko_set_timestamp(ctx, options.time_offset_plus);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_timestamp", res);
            clean_exit(ctx, &options, key, &key_len,
                hmac_key, &hmac_key_len, EXIT_FAILURE);
        }
    }
    if(options.time_offset_minus > 0)
    {
        res = fko_set_timestamp(ctx, -options.time_offset_minus);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_timestamp", res);
            clean_exit(ctx, &options, key, &key_len,
                hmac_key, &hmac_key_len, EXIT_FAILURE);
        }
    }

    if(options.server_command[0] != 0x0)
    {
        /* Set the access message to a command that the server will
         * execute
        */
        snprintf(access_buf, MAX_LINE_LEN, "%s%s%s",
                options.allow_ip_str, ",", options.server_command);
    }
    else
    {
        /* Resolve the client's public facing IP address if requestesd.
         * if this fails, consider it fatal.
        */
        if (options.resolve_ip_http)
        {
            if(resolve_ip_http(&options) < 0)
            {
                clean_exit(ctx, &options, key, &key_len,
                    hmac_key, &hmac_key_len, EXIT_FAILURE);
            }
        }

        /* Set a message string by combining the allow IP and the
         * port/protocol.  The fwknopd server allows no port/protocol
         * to be specified as well, so in this case append the string
         * "none/0" to the allow IP.
        */
        if(set_access_buf(ctx, &options, access_buf) != 1)
            clean_exit(ctx, &options, key, &key_len,
                    hmac_key, &hmac_key_len, EXIT_FAILURE);
    }
    res = fko_set_spa_message(ctx, access_buf);
    if(res != FKO_SUCCESS)
    {
        errmsg("fko_set_spa_message", res);
        clean_exit(ctx, &options, key, &key_len,
            hmac_key, &hmac_key_len, EXIT_FAILURE);
    }

    /* Set NAT access string
    */
    if (options.nat_local || options.nat_access_str[0] != 0x0)
    {
        res = set_nat_access(ctx, &options, access_buf);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_nat_access_str", res);
            clean_exit(ctx, &options, key, &key_len,
                    hmac_key, &hmac_key_len, EXIT_FAILURE);
        }
    }

    /* Set username
    */
    if(options.spoof_user[0] != 0x0)
    {
        res = fko_set_username(ctx, options.spoof_user);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_username", res);
            clean_exit(ctx, &options, key, &key_len,
                    hmac_key, &hmac_key_len, EXIT_FAILURE);
        }
    }

    /* Handle legacy digits-only random data for backwards
     * compatibility only - this is not the default
    */
    if(options.rand_mode_legacy)
    {
        res = fko_set_rand_mode(ctx, FKO_RAND_MODE_LEGACY);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_rand_mode", res);
            clean_exit(ctx, &options, key, &key_len,
                    hmac_key, &hmac_key_len, EXIT_FAILURE);
        }
        res = fko_set_rand_value(ctx, NULL);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_rand_value", res);
            clean_exit(ctx, &options, key, &key_len,
                    hmac_key, &hmac_key_len, EXIT_FAILURE);
        }
    }

    /* Set up for using GPG if specified.
    */
    if(options.use_gpg)
    {
        /* If use-gpg-agent was not specified, then remove the GPG_AGENT_INFO
         * ENV variable if it exists.
        */
#ifndef WIN32
        if(!options.use_gpg_agent)
            unsetenv("GPG_AGENT_INFO");
#endif

        res = fko_set_spa_encryption_type(ctx, FKO_ENCRYPTION_GPG);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_spa_encryption_type", res);
            clean_exit(ctx, &options, key, &key_len,
                    hmac_key, &hmac_key_len, EXIT_FAILURE);
        }

        /* Set gpg path if necessary
        */
        if(strlen(options.gpg_exe) > 0)
        {
            res = fko_set_gpg_exe(ctx, options.gpg_exe);
            if(res != FKO_SUCCESS)
            {
                errmsg("fko_set_gpg_exe", res);
                clean_exit(ctx, &options, key, &key_len,
                        hmac_key, &hmac_key_len, EXIT_FAILURE);
            }
        }

        /* If a GPG home dir was specified, set it here.  Note: Setting
         * this has to occur before calling any of the other GPG-related
         * functions.
        */
        if(strlen(options.gpg_home_dir) > 0)
        {
            res = fko_set_gpg_home_dir(ctx, options.gpg_home_dir);
            if(res != FKO_SUCCESS)
            {
                errmsg("fko_set_gpg_home_dir", res);
                clean_exit(ctx, &options, key, &key_len,
                        hmac_key, &hmac_key_len, EXIT_FAILURE);
            }
        }

        res = fko_set_gpg_recipient(ctx, options.gpg_recipient_key);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_gpg_recipient", res);

            if(IS_GPG_ERROR(res))
                log_msg(LOG_VERBOSITY_ERROR, "GPG ERR: %s", fko_gpg_errstr(ctx));
            clean_exit(ctx, &options, key, &key_len,
                    hmac_key, &hmac_key_len, EXIT_FAILURE);
        }

        if(strlen(options.gpg_signer_key) > 0)
        {
            res = fko_set_gpg_signer(ctx, options.gpg_signer_key);
            if(res != FKO_SUCCESS)
            {
                errmsg("fko_set_gpg_signer", res);

                if(IS_GPG_ERROR(res))
                    log_msg(LOG_VERBOSITY_ERROR, "GPG ERR: %s", fko_gpg_errstr(ctx));
                clean_exit(ctx, &options, key, &key_len,
                        hmac_key, &hmac_key_len, EXIT_FAILURE);
            }
        }

        res = fko_set_spa_encryption_mode(ctx, FKO_ENC_MODE_ASYMMETRIC);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_spa_encryption_mode", res);
            clean_exit(ctx, &options, key, &key_len,
                    hmac_key, &hmac_key_len, EXIT_FAILURE);
        }
    }

    if(options.encryption_mode && !options.use_gpg)
    {
        res = fko_set_spa_encryption_mode(ctx, options.encryption_mode);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_spa_encryption_mode", res);
            clean_exit(ctx, &options, key, &key_len,
                    hmac_key, &hmac_key_len, EXIT_FAILURE);
        }
    }

    /* Set Digest type.
    */
    if(options.digest_type)
    {
        res = fko_set_spa_digest_type(ctx, options.digest_type);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_spa_digest_type", res);
            clean_exit(ctx, &options, key, &key_len,
                    hmac_key, &hmac_key_len, EXIT_FAILURE);
        }
    }

    /* Acquire the necessary encryption/hmac keys
    */
    if(get_keys(ctx, &options, key, &key_len, hmac_key, &hmac_key_len) != 1)
        clean_exit(ctx, &options, key, &key_len,
                hmac_key, &hmac_key_len, EXIT_FAILURE);

    orig_key_len = key_len;

    if(options.encryption_mode == FKO_ENC_MODE_CBC_LEGACY_IV
            && key_len > 16)
    {
        log_msg(LOG_VERBOSITY_ERROR,
                "WARNING: Encryption key in '-M legacy' mode must be <= 16 bytes");
        log_msg(LOG_VERBOSITY_ERROR,
                "long - truncating before sending SPA packet. Upgrading remote");
        log_msg(LOG_VERBOSITY_ERROR,
                "fwknopd is recommended.");
        key_len = 16;
    }

    /* Finalize the context data (encrypt and encode the SPA data)
    */
    res = fko_spa_data_final(ctx, key, key_len, hmac_key, hmac_key_len);
    if(res != FKO_SUCCESS)
    {
        errmsg("fko_spa_data_final", res);

        if(IS_GPG_ERROR(res))
            log_msg(LOG_VERBOSITY_ERROR, "GPG ERR: %s", fko_gpg_errstr(ctx));
        clean_exit(ctx, &options, key, &orig_key_len,
                hmac_key, &hmac_key_len, EXIT_FAILURE);
    }

    /* Display the context data.
    */
    if (options.verbose || options.test)
    {
        res = dump_ctx_to_buffer(ctx, dump_buf, sizeof(dump_buf));
        if (res == FKO_SUCCESS)
            log_msg(LOG_VERBOSITY_NORMAL, "%s", dump_buf);
        else
            log_msg(LOG_VERBOSITY_WARNING, "Unable to dump FKO context: %s", fko_errstr(res));
    }

    /* Save packet data payload if requested.
    */
    if (options.save_packet_file[0] != 0x0)
        write_spa_packet_data(ctx, &options);

    /* SPA packet random destination port handling
    */
    if (options.rand_port)
    {
        tmp_port = get_rand_port();
        if(tmp_port < 0)
            clean_exit(ctx, &options, key, &orig_key_len,
                    hmac_key, &hmac_key_len, EXIT_FAILURE);
        options.spa_dst_port = tmp_port;
    }

    /* If we are using one the "raw" modes (normally because
     * we're going to spoof the SPA packet source IP), then select
     * a random source port unless the source port is already set
    */
    if ((options.spa_proto == FKO_PROTO_TCP_RAW
            || options.spa_proto == FKO_PROTO_UDP_RAW
            || options.spa_proto == FKO_PROTO_ICMP)
            && !options.spa_src_port)
    {
        tmp_port = get_rand_port();
        if(tmp_port < 0)
            clean_exit(ctx, &options, key, &orig_key_len,
                    hmac_key, &hmac_key_len, EXIT_FAILURE);
        options.spa_src_port = tmp_port;
    }

    res = send_spa_packet(ctx, &options);
    if(res < 0)
    {
        log_msg(LOG_VERBOSITY_ERROR, "send_spa_packet: packet not sent.");
        clean_exit(ctx, &options, key, &orig_key_len,
                hmac_key, &hmac_key_len, EXIT_FAILURE);
    }
    else
    {
        log_msg(LOG_VERBOSITY_INFO, "send_spa_packet: bytes sent: %i", res);
    }

    /* Run through a decode cycle in test mode (--DSS XXX: This test/decode
     * portion should be moved elsewhere).
    */
    if (options.test)
    {
        /************** Decoding now *****************/

        /* Now we create a new context based on data from the first one.
        */
        res = fko_get_spa_data(ctx, &spa_data);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_get_spa_data", res);
            clean_exit(ctx, &options, key, &orig_key_len,
                hmac_key, &hmac_key_len, EXIT_FAILURE);
        }

        /* Pull the encryption mode.
        */
        res = fko_get_spa_encryption_mode(ctx, &enc_mode);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_get_spa_encryption_mode", res);
            if(fko_destroy(ctx2) == FKO_ERROR_ZERO_OUT_DATA)
                log_msg(LOG_VERBOSITY_ERROR,
                        "[*] Could not zero out sensitive data buffer.");
            ctx2 = NULL;
            clean_exit(ctx, &options, key, &orig_key_len,
                hmac_key, &hmac_key_len, EXIT_FAILURE);
        }

        /* If gpg-home-dir is specified, we have to defer decrypting if we
         * use the fko_new_with_data() function because we need to set the
         * gpg home dir after the context is created, but before we attempt
         * to decrypt the data.  Therefore we either pass NULL for the
         * decryption key to fko_new_with_data() or use fko_new() to create
         * an empty context, populate it with the encrypted data, set our
         * options, then decode it.
         *
         * This also verifies the HMAC and truncates it if there are no
         * problems.
        */
        res = fko_new_with_data(&ctx2, spa_data, NULL,
            0, enc_mode, hmac_key, hmac_key_len, options.hmac_type,
            rand_mode);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_new_with_data", res);
            if(fko_destroy(ctx2) == FKO_ERROR_ZERO_OUT_DATA)
                log_msg(LOG_VERBOSITY_ERROR,
                        "[*] Could not zero out sensitive data buffer.");
            ctx2 = NULL;
            clean_exit(ctx, &options, key, &orig_key_len,
                hmac_key, &hmac_key_len, EXIT_FAILURE);
        }

        res = fko_set_spa_encryption_mode(ctx2, enc_mode);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_spa_encryption_mode", res);
            if(fko_destroy(ctx2) == FKO_ERROR_ZERO_OUT_DATA)
                log_msg(LOG_VERBOSITY_ERROR,
                        "[*] Could not zero out sensitive data buffer.");
            ctx2 = NULL;
            clean_exit(ctx, &options, key, &orig_key_len,
                hmac_key, &hmac_key_len, EXIT_FAILURE);
        }

        if(options.rand_mode_legacy)
        {
            res = fko_set_rand_mode(ctx2, FKO_RAND_MODE_LEGACY);
            if(res != FKO_SUCCESS)
            {
                if(fko_destroy(ctx2) == FKO_ERROR_ZERO_OUT_DATA)
                    log_msg(LOG_VERBOSITY_ERROR,
                            "[*] Could not zero out sensitive data buffer.");
                ctx2 = NULL;
                errmsg("fko_set_rand_mode", res);
                clean_exit(ctx, &options, key, &key_len,
                        hmac_key, &hmac_key_len, EXIT_FAILURE);
            }
        }

        /* See if we are using gpg and if we need to set the GPG home dir.
        */
        if(options.use_gpg)
        {
            if(strlen(options.gpg_home_dir) > 0)
            {
                res = fko_set_gpg_home_dir(ctx2, options.gpg_home_dir);
                if(res != FKO_SUCCESS)
                {
                    errmsg("fko_set_gpg_home_dir", res);
                    if(fko_destroy(ctx2) == FKO_ERROR_ZERO_OUT_DATA)
                        log_msg(LOG_VERBOSITY_ERROR,
                                "[*] Could not zero out sensitive data buffer.");
                    ctx2 = NULL;
                    clean_exit(ctx, &options, key, &orig_key_len,
                        hmac_key, &hmac_key_len, EXIT_FAILURE);
                }
            }
        }

        /* Decrypt
        */
        res = fko_decrypt_spa_data(ctx2, key, key_len);

        if(res != FKO_SUCCESS)
        {
            errmsg("fko_decrypt_spa_data", res);

            if(IS_GPG_ERROR(res)) {
                /* we most likely could not decrypt the gpg-encrypted data
                 * because we don't have access to the private key associated
                 * with the public key we used for encryption.  Since this is
                 * expected, return 0 instead of an error condition (so calling
                 * programs like the fwknop test suite don't interpret this as
                 * an unrecoverable error), but print the error string for
                 debugging purposes. */
                log_msg(LOG_VERBOSITY_ERROR, "GPG ERR: %s\n%s", fko_gpg_errstr(ctx2),
                    "No access to recipient private key?");
            }
            if(fko_destroy(ctx2) == FKO_ERROR_ZERO_OUT_DATA)
                log_msg(LOG_VERBOSITY_ERROR,
                        "[*] Could not zero out sensitive data buffer.");
            ctx2 = NULL;
            clean_exit(ctx, &options, key, &orig_key_len,
                hmac_key, &hmac_key_len, EXIT_FAILURE);
        }

        res = dump_ctx_to_buffer(ctx2, dump_buf, sizeof(dump_buf));
        if (res == FKO_SUCCESS)
            log_msg(LOG_VERBOSITY_NORMAL, "\nDump of the Decoded Data\n%s", dump_buf);
        else
            log_msg(LOG_VERBOSITY_WARNING, "Unable to dump FKO context: %s", fko_errstr(res));

        if(fko_destroy(ctx2) == FKO_ERROR_ZERO_OUT_DATA)
            log_msg(LOG_VERBOSITY_ERROR,
                    "[*] Could not zero out sensitive data buffer.");
        ctx2 = NULL;
    }

    clean_exit(ctx, &options, key, &orig_key_len,
            hmac_key, &hmac_key_len, EXIT_SUCCESS);

    return EXIT_SUCCESS;  /* quiet down a gcc warning */
}

void
free_configs(fko_cli_options_t *opts)
{
    if (opts->resolve_url != NULL)
        free(opts->resolve_url);
    zero_buf_wrapper(opts->key, MAX_KEY_LEN+1);
    zero_buf_wrapper(opts->key_base64, MAX_B64_KEY_LEN+1);
    zero_buf_wrapper(opts->hmac_key, MAX_KEY_LEN+1);
    zero_buf_wrapper(opts->hmac_key_base64, MAX_B64_KEY_LEN+1);
    zero_buf_wrapper(opts->gpg_recipient_key, MAX_GPG_KEY_ID);
    zero_buf_wrapper(opts->gpg_signer_key, MAX_GPG_KEY_ID);
    zero_buf_wrapper(opts->gpg_home_dir, MAX_PATH_LEN);
    zero_buf_wrapper(opts->server_command, MAX_LINE_LEN);
}

static int
get_rand_port(void)
{
    unsigned char  port_str[MAX_PORT_STR_LEN+1] = {0};
    int            tmpint, is_err;
    int            res = 0;

    res = fko_rand_data(port_str, MAX_PORT_STR_LEN+1,
            FKO_RAND_MODE_LEGACY);  /* digits only */
    if(res != FKO_SUCCESS)
    {
        errmsg("get_rand_port(), fko_get_rand_value", res);
        return -1;
    }
    port_str[MAX_PORT_STR_LEN] = '\0';

    tmpint = strtol_wrapper((char *) port_str, 0, -1, NO_EXIT_UPON_ERR, &is_err);
    if(is_err != FKO_SUCCESS)
    {
        log_msg(LOG_VERBOSITY_ERROR,
            "[*] get_rand_port(), could not convert port str '%s', to integer",
            port_str);
        return -1;
    }

    /* Convert to a random value between 10000 and 65535
    */
    return (MIN_HIGH_PORT + (tmpint % (MAX_PORT - MIN_HIGH_PORT)));
}

/* See if the string is of the format "<ipv4 addr>:<port>",
 */
static int
ipv4_str_has_port(char *str)
{
    int o1, o2, o3, o4, p;

    /* Force the ':' (if any) to a ','
    */
    char *ndx = strchr(str, ':');
    if(ndx != NULL)
        *ndx = ',';

    /* Check format and values.
    */
    if((sscanf(str, "%u.%u.%u.%u,%u", &o1, &o2, &o3, &o4, &p)) == 5
        && o1 >= 0 && o1 <= 255
        && o2 >= 0 && o2 <= 255
        && o3 >= 0 && o3 <= 255
        && o4 >= 0 && o4 <= 255
        && p  >  0 && p  <  65536)
    {
        return 1;
    }

    return 0;
}

/* Set access buf
*/
static int
set_access_buf(fko_ctx_t ctx, fko_cli_options_t *options, char *access_buf)
{
    char   *ndx = NULL, tmp_nat_port[MAX_PORT_STR_LEN+1] = {0};
    int     nat_port = 0;

    if(options->access_str[0] != 0x0)
    {
        if (options->nat_rand_port)
        {
            nat_port = get_rand_port();
            options->nat_port = nat_port;
        }
        else if (options->nat_port)
            nat_port = options->nat_port;

        if(nat_port > 0 && nat_port <= MAX_PORT)
        {
            /* Replace the access string port with the NAT port since the
             * NAT port is manually specified (--nat-port) or derived from
             * random data (--nat-rand-port).  In the NAT modes, the fwknopd
             * server uses the port in the access string as the one to NAT,
             * and access is granted via this translated port to whatever is
             * specified with --nat-access <IP:port> (so this service is the
             * utlimate target of the incoming connection after the SPA
             * packet is sent).
            */
            ndx = strchr(options->access_str, '/');
            if(ndx == NULL)
            {
                log_msg(LOG_VERBOSITY_ERROR, "[*] Expecting <proto>/<port> for -A arg.");
                return 0;
            }
            snprintf(access_buf, MAX_LINE_LEN, "%s%s",
                    options->allow_ip_str, ",");

            /* This adds in the protocol + '/' char
            */
            strlcat(access_buf, options->access_str,
                    strlen(access_buf) + (ndx - options->access_str) + 2);

            if (strchr(ndx+1, '/') != NULL)
            {
                log_msg(LOG_VERBOSITY_ERROR,
                        "[*] NAT for multiple ports/protocols not yet supported.");
                return 0;
            }

            /* Now add the NAT port
            */
            snprintf(tmp_nat_port, MAX_PORT_STR_LEN+1, "%d", nat_port);
            strlcat(access_buf, tmp_nat_port,
                    strlen(access_buf)+MAX_PORT_STR_LEN+1);
        }
        else
        {
            snprintf(access_buf, MAX_LINE_LEN, "%s%s%s",
                    options->allow_ip_str, ",", options->access_str);
        }
    }
    else
    {
        snprintf(access_buf, MAX_LINE_LEN, "%s%s%s",
                options->allow_ip_str, ",", "none/0");
    }
    return 1;
}

/* Set NAT access string
*/
static int
set_nat_access(fko_ctx_t ctx, fko_cli_options_t *options, const char * const access_buf)
{
    char                nat_access_buf[MAX_LINE_LEN] = {0};
    char                tmp_access_port[MAX_PORT_STR_LEN+1] = {0}, *ndx = NULL;
    int                 access_port = 0, i = 0, is_err = 0;
    char                dst_ip_str[INET_ADDRSTRLEN] = {0};
    char                hostname[HOSTNAME_BUFSIZE] = {0};
    int                 port = 0;
    struct addrinfo     hints;

    memset(&hints, 0 , sizeof(hints));

    ndx = strchr(options->access_str, '/');
    if(ndx == NULL)
    {
        log_msg(LOG_VERBOSITY_ERROR, "[*] Expecting <proto>/<port> for -A arg.");
        return FKO_ERROR_INVALID_DATA;
    }
    ndx++;

    while(*ndx != '\0' && isdigit(*ndx) && i < MAX_PORT_STR_LEN)
    {
        tmp_access_port[i] = *ndx;
        ndx++;
        i++;
    }
    tmp_access_port[i] = '\0';

    access_port = strtol_wrapper(tmp_access_port, 1,
            MAX_PORT, NO_EXIT_UPON_ERR, &is_err);
    if(is_err != FKO_SUCCESS)
    {
        log_msg(LOG_VERBOSITY_ERROR, "[*] Invalid port value '%d' for -A arg.",
                access_port);
        return FKO_ERROR_INVALID_DATA;
    }

    if (options->nat_local && options->nat_access_str[0] == 0x0)
    {
        snprintf(nat_access_buf, MAX_LINE_LEN, NAT_ACCESS_STR_TEMPLATE,
            options->spa_server_str, access_port);
    }

    if (nat_access_buf[0] == 0x0 && options->nat_access_str[0] != 0x0)
    {
        if (ipv4_str_has_port(options->nat_access_str))
        {
            snprintf(nat_access_buf, MAX_LINE_LEN, "%s",
                options->nat_access_str);
        }
        else
        {
            snprintf(nat_access_buf, MAX_LINE_LEN, NAT_ACCESS_STR_TEMPLATE,
                options->nat_access_str, access_port);
        }
    }

    /* Check if there is a hostname to resolve as an ip address in the NAT access buffer */
    if (is_hostname_str_with_port(nat_access_buf, hostname, sizeof(hostname), &port))
    {
        /* Speed up the name resolution by forcing ipv4 (AF_INET).
         * A NULL pointer could be used instead if there is no constraint.
         * Maybe when ipv6 support will be enable the structure could initialize the
         * family to either AF_INET or AF_INET6 */
        hints.ai_family = AF_INET;

        if (resolve_dest_adr(hostname, &hints, dst_ip_str, sizeof(dst_ip_str)) != 0)
        {
            log_msg(LOG_VERBOSITY_ERROR, "[*] Unable to resolve %s as an ip address",
                    hostname);
            return FKO_ERROR_INVALID_DATA;
        }

        snprintf(nat_access_buf, MAX_LINE_LEN, NAT_ACCESS_STR_TEMPLATE,
                dst_ip_str, port);
    }

    /* Nothing to resolve */
    else;

    if(options->nat_rand_port)
    {
        /* Must print to stdout what the random port is since
         * if not then the user will not which port will be
         * opened/NAT'd on the fwknopd side
        */
        log_msg(LOG_VERBOSITY_NORMAL,
                "[+] Randomly assigned port '%d' on: '%s' will grant access to: '%s'",
                options->nat_port, access_buf, nat_access_buf);
    }

    return fko_set_spa_nat_access(ctx, nat_access_buf);
}

static int
prev_exec(fko_cli_options_t *options, int argc, char **argv)
{
    char       args_save_file[MAX_PATH_LEN] = {0};
    int        res = 1;

    if(options->args_save_file[0] != 0x0)
    {
        strlcpy(args_save_file, options->args_save_file, sizeof(args_save_file));
    }
    else
    {
        if (get_save_file(args_save_file) != 1)
        {
            log_msg(LOG_VERBOSITY_ERROR, "Unable to determine args save file");
            return 0;
        }
    }

    if(options->run_last_command)
        res = run_last_args(options, args_save_file);
    else if(options->show_last_command)
        res = show_last_command(args_save_file);
    else if (!options->no_save_args)
        res = save_args(argc, argv, args_save_file);

    return res;
}

/* Show the last command that was executed
*/
static int
show_last_command(const char * const args_save_file)
{
    char args_str[MAX_LINE_LEN] = {0};
    FILE *args_file_ptr = NULL;

    if(verify_file_perms_ownership(args_save_file) != 1)
        return 0;

    if ((args_file_ptr = fopen(args_save_file, "r")) == NULL) {
        log_msg(LOG_VERBOSITY_ERROR, "Could not open args file: %s",
            args_save_file);
        return 0;
    }

    if ((fgets(args_str, MAX_LINE_LEN, args_file_ptr)) != NULL) {
        log_msg(LOG_VERBOSITY_NORMAL, "Last fwknop client command line: %s", args_str);
    } else {
        log_msg(LOG_VERBOSITY_NORMAL, "Could not read line from file: %s", args_save_file);
    }
    fclose(args_file_ptr);

    return 1;
}

/* Get the command line arguments from the previous invocation
*/
static int
run_last_args(fko_cli_options_t *options, const char * const args_save_file)
{
    FILE           *args_file_ptr = NULL;

    int             current_arg_ctr = 0;
    int             argc_new = 0;
    int             i = 0;

    char            args_str[MAX_LINE_LEN] = {0};
    char            arg_tmp[MAX_LINE_LEN]  = {0};
    char           *argv_new[MAX_CMDLINE_ARGS];  /* should be way more than enough */

    if(verify_file_perms_ownership(args_save_file) != 1)
        return 0;

    if ((args_file_ptr = fopen(args_save_file, "r")) == NULL)
    {
        log_msg(LOG_VERBOSITY_ERROR, "Could not open args file: %s",
                args_save_file);
        return 0;
    }
    if ((fgets(args_str, MAX_LINE_LEN, args_file_ptr)) != NULL)
    {
        args_str[MAX_LINE_LEN-1] = '\0';
        if (options->verbose)
            log_msg(LOG_VERBOSITY_NORMAL, "Executing: %s", args_str);
        for (i=0; i < (int)strlen(args_str); i++)
        {
            if (!isspace(args_str[i]))
            {
                arg_tmp[current_arg_ctr] = args_str[i];
                current_arg_ctr++;
            }
            else
            {
                arg_tmp[current_arg_ctr] = '\0';
                argv_new[argc_new] = calloc(1, strlen(arg_tmp)+1);
                if (argv_new[argc_new] == NULL)
                {
                    log_msg(LOG_VERBOSITY_ERROR, "[*] calloc failure for cmd line arg.");
                    fclose(args_file_ptr);
                    return 0;
                }
                strlcpy(argv_new[argc_new], arg_tmp, strlen(arg_tmp)+1);
                current_arg_ctr = 0;
                argc_new++;
                if(argc_new >= MAX_CMDLINE_ARGS)
                {
                    log_msg(LOG_VERBOSITY_ERROR, "[*] max command line args exceeded.");
                    fclose(args_file_ptr);
                    return 0;
                }
            }
        }
    }
    fclose(args_file_ptr);

    /* Reset the options index so we can run through them again.
    */
    optind = 0;

    config_init(options, argc_new, argv_new);

    /* Since we passed in our own copies, free up malloc'd memory
    */
    for (i=0; i < argc_new; i++)
    {
        if(argv_new[i] == NULL)
            break;
        else
            free(argv_new[i]);
    }

    return 1;
}

static int
get_save_file(char *args_save_file)
{
    char *homedir = NULL;
    int rv = 0;

#ifdef WIN32
    homedir = getenv("USERPROFILE");
#else
    homedir = getenv("HOME");
#endif
    if (homedir != NULL) {
        snprintf(args_save_file, MAX_PATH_LEN, "%s%c%s",
            homedir, PATH_SEP, ".fwknop.run");
        rv = 1;
    }

    return rv;
}

/* Save our command line arguments
*/
static int
save_args(int argc, char **argv, const char * const args_save_file)
{
    char args_str[MAX_LINE_LEN] = {0};
    int i = 0, args_str_len = 0, args_file_fd = -1;

    args_file_fd = open(args_save_file, O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR);
    if (args_file_fd == -1) {
        log_msg(LOG_VERBOSITY_ERROR, "Could not open args file: %s",
            args_save_file);
        return 0;
    }
    else {
        for (i=0; i < argc; i++) {
            args_str_len += strlen(argv[i]);
            if (args_str_len >= MAX_PATH_LEN) {
                log_msg(LOG_VERBOSITY_ERROR, "argument string too long, exiting.");
                close(args_file_fd);
                return 0;
            }
            strlcat(args_str, argv[i], sizeof(args_str));
            strlcat(args_str, " ", sizeof(args_str));
        }
        strlcat(args_str, "\n", sizeof(args_str));
        if(write(args_file_fd, args_str, strlen(args_str))
                != strlen(args_str)) {
            log_msg(LOG_VERBOSITY_WARNING,
                "warning, did not write expected number of bytes to args save file");
        }
        close(args_file_fd);
    }
    return 1;
}

/* Set the SPA packet message type
*/
static int
set_message_type(fko_ctx_t ctx, fko_cli_options_t *options)
{
    short message_type;

    if(options->server_command[0] != 0x0)
    {
        message_type = FKO_COMMAND_MSG;
    }
    else if(options->nat_local)
    {
        if (options->fw_timeout >= 0)
            message_type = FKO_CLIENT_TIMEOUT_LOCAL_NAT_ACCESS_MSG;
        else
            message_type = FKO_LOCAL_NAT_ACCESS_MSG;
    }
    else if(options->nat_access_str[0] != 0x0)
    {
        if (options->fw_timeout >= 0)
            message_type = FKO_CLIENT_TIMEOUT_NAT_ACCESS_MSG;
        else
            message_type = FKO_NAT_ACCESS_MSG;
    }
    else
    {
        if (options->fw_timeout >= 0)
            message_type = FKO_CLIENT_TIMEOUT_ACCESS_MSG;
        else
            message_type = FKO_ACCESS_MSG;
    }

    return fko_set_spa_message_type(ctx, message_type);
}

/* Prompt for and receive a user password.
*/
static int
get_keys(fko_ctx_t ctx, fko_cli_options_t *options,
    char *key, int *key_len, char *hmac_key, int *hmac_key_len)
{
    char   *key_tmp = NULL, *hmac_key_tmp = NULL;
    int     use_hmac = 0, res = 0;

    memset(key, 0x0, MAX_KEY_LEN+1);
    memset(hmac_key, 0x0, MAX_KEY_LEN+1);

    if(options->have_key)
    {
        strlcpy(key, options->key, MAX_KEY_LEN+1);
        *key_len = strlen(key);
    }
    else if(options->have_base64_key)
    {
        *key_len = fko_base64_decode(options->key_base64,
                (unsigned char *) options->key);
        if(*key_len > 0 && *key_len < MAX_KEY_LEN)
        {
            memcpy(key, options->key, *key_len);
        }
        else
        {
            log_msg(LOG_VERBOSITY_ERROR, "[*] Invalid key length: '%d', must be in [1,%d]",
                    *key_len, MAX_KEY_LEN);
            return 0;
        }
    }
    else
    {
        /* If --get-key file was specified grab the key/password from it.
        */
        if(options->get_key_file[0] != 0x0)
        {
            if(get_key_file(key, key_len, options->get_key_file, ctx, options) != 1)
            {
                return 0;
            }
        }
        else if(options->use_gpg)
        {
            if(options->use_gpg_agent)
                log_msg(LOG_VERBOSITY_NORMAL,
                    "[+] GPG mode set, signing passphrase acquired via gpg-agent");
            else if(options->gpg_no_signing_pw)
                log_msg(LOG_VERBOSITY_NORMAL,
                    "[+] GPG mode set, signing passphrase not required");
            else if(strlen(options->gpg_signer_key))
            {
                key_tmp = getpasswd("Enter passphrase for signing: ", options->input_fd);
                if(key_tmp == NULL)
                {
                    log_msg(LOG_VERBOSITY_ERROR, "[*] getpasswd() key error.");
                    return 0;
                }
                strlcpy(key, key_tmp, MAX_KEY_LEN+1);
                *key_len = strlen(key);
            }
        }
        else
        {
            key_tmp = getpasswd("Enter encryption key: ", options->input_fd);

            if(key_tmp == NULL)
            {
                log_msg(LOG_VERBOSITY_ERROR, "[*] getpasswd() key error.");
                return 0;
            }
            strlcpy(key, key_tmp, MAX_KEY_LEN+1);
            *key_len = strlen(key);
        }
    }

    if(options->have_hmac_key)
    {
        strlcpy(hmac_key, options->hmac_key, MAX_KEY_LEN+1);
        *hmac_key_len = strlen(hmac_key);
        use_hmac = 1;
    }
    else if(options->have_hmac_base64_key)
    {
        *hmac_key_len = fko_base64_decode(options->hmac_key_base64,
            (unsigned char *) options->hmac_key);
        if(*hmac_key_len > MAX_KEY_LEN || *hmac_key_len < 0)
        {
            log_msg(LOG_VERBOSITY_ERROR,
                    "[*] Invalid decoded key length: '%d', must be in [0,%d]",
                    *hmac_key_len, MAX_KEY_LEN);
            return 0;
        }
        memcpy(hmac_key, options->hmac_key, *hmac_key_len);
        use_hmac = 1;
    }
    else if (options->use_hmac)
    {
        /* If --get-key file was specified grab the key/password from it.
        */
        if(options->get_hmac_key_file[0] != 0x0)
        {
            if(get_key_file(hmac_key, hmac_key_len,
                options->get_hmac_key_file, ctx, options) != 1)
            {
                return 0;
            }
            use_hmac = 1;
        }
        else
        {
            hmac_key_tmp = getpasswd("Enter HMAC key: ", options->input_fd);

            if(hmac_key_tmp == NULL)
            {
                log_msg(LOG_VERBOSITY_ERROR, "[*] getpasswd() key error.");
                return 0;
            }

            strlcpy(hmac_key, hmac_key_tmp, MAX_KEY_LEN+1);
            *hmac_key_len = strlen(hmac_key);
            use_hmac = 1;
        }
    }

    if (use_hmac)
    {
        if(*hmac_key_len < 0 || *hmac_key_len > MAX_KEY_LEN)
        {
            log_msg(LOG_VERBOSITY_ERROR, "[*] Invalid HMAC key length: '%d', must be in [0,%d]",
                    *hmac_key_len, MAX_KEY_LEN);
            return 0;
        }

        /* Make sure the same key is not used for both encryption and the HMAC
        */
        if(*hmac_key_len == *key_len)
        {
            if(memcmp(hmac_key, key, *key_len) == 0)
            {
                log_msg(LOG_VERBOSITY_ERROR,
                    "[*] The encryption passphrase and HMAC key should not be identical, no SPA packet sent. Exiting.");
                return 0;
            }
        }

        res = fko_set_spa_hmac_type(ctx, options->hmac_type);
        if(res != FKO_SUCCESS)
        {
            errmsg("fko_set_spa_hmac_type", res);
            return 0;
        }
    }

    return 1;
}

/* Display an FKO error message.
*/
void
errmsg(const char *msg, const int err) {
    log_msg(LOG_VERBOSITY_ERROR, "%s: %s: Error %i - %s",
        MY_NAME, msg, err, fko_errstr(err));
}

static void
zero_buf_wrapper(char *buf, int len)
{

    if(buf == NULL || len == 0)
        return;

    if(zero_buf(buf, len) == FKO_ERROR_ZERO_OUT_DATA)
        log_msg(LOG_VERBOSITY_ERROR,
                "[*] Could not zero out sensitive data buffer.");

    return;
}

/* free up memory and exit
*/
static void
clean_exit(fko_ctx_t ctx, fko_cli_options_t *opts,
        char *key, int *key_len, char *hmac_key, int *hmac_key_len,
        unsigned int exit_status)
{
    if(fko_destroy(ctx) == FKO_ERROR_ZERO_OUT_DATA)
        log_msg(LOG_VERBOSITY_ERROR,
                "[*] Could not zero out sensitive data buffer.");
    ctx = NULL;
    free_configs(opts);
    zero_buf_wrapper(key, *key_len);
    zero_buf_wrapper(hmac_key, *hmac_key_len);
    *key_len = 0;
    *hmac_key_len = 0;
    exit(exit_status);
}

/***EOF***/
