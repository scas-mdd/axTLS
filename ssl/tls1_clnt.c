/*
 *  Copyright(C) 2006 Cameron Rich
 *
 *  This library is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#include "ssl.h"

#ifdef CONFIG_SSL_ENABLE_CLIENT        /* all commented out if no client */

static int send_client_hello(SSL *ssl);
static int process_server_hello(SSL *ssl);
static int process_server_hello_done(SSL *ssl);
static int send_client_key_xchg(SSL *ssl);
static int process_cert_req(SSL *ssl);
static int send_cert_verify(SSL *ssl);

/*
 * Establish a new SSL connection to an SSL server.
 */
EXP_FUNC SSL * STDCALL ssl_client_new(SSL_CTX *ssl_ctx, int client_fd, const uint8_t *session_id)
{
    SSL *ssl;
    int ret;

    SOCKET_BLOCK(client_fd);    /* ensure blocking mode */
    ssl = ssl_new(ssl_ctx, client_fd);

    if (session_id && ssl_ctx->num_sessions)
    {
        memcpy(ssl->session_id, session_id, SSL_SESSION_ID_SIZE);
        SET_SSL_FLAG(SSL_SESSION_RESUME);   /* just flag for later */
    }

    SET_SSL_FLAG(SSL_IS_CLIENT);
    ret = do_client_connect(ssl);
    return ssl;
}

/*
 * Process the handshake record.
 */
int do_clnt_handshake(SSL *ssl, int handshake_type, uint8_t *buf, int hs_len)
{
    int ret = SSL_OK;

    /* To get here the state must be valid */
    switch (handshake_type)
    {
        case HS_SERVER_HELLO:
            ret = process_server_hello(ssl);
            break;

        case HS_CERTIFICATE:
            ret = process_certificate(ssl, &ssl->x509_ctx);
            break;

        case HS_SERVER_HELLO_DONE:
            if ((ret = process_server_hello_done(ssl)) == SSL_OK)
            {
                if (IS_SET_SSL_FLAG(SSL_HAS_CERT_REQ))
                {
                    if ((ret = send_certificate(ssl)) == SSL_OK &&
                        (ret = send_client_key_xchg(ssl)) == SSL_OK)
                    {
                        ret = send_cert_verify(ssl);
                    }
                }
                else
                {
                    ret = send_client_key_xchg(ssl);
                }

                if (ret == SSL_OK && 
                     (ret = send_change_cipher_spec(ssl)) == SSL_OK)
                {
                    ret = send_finished(ssl);
                }
            }
            break;

        case HS_CERT_REQ:
            ret = process_cert_req(ssl);
            break;

        case HS_FINISHED:
            ret = process_finished(ssl, hs_len);
            break;

        case HS_HELLO_REQUEST:
            ret = do_client_connect(ssl);
            break;
    }

    return ret;
}

/*
 * Do the handshaking from the beginning.
 */
int do_client_connect(SSL *ssl)
{
    int ret = SSL_OK;

    send_client_hello(ssl);                 /* send the client hello */
    ssl->bm_read_index = 0;
    ssl->next_state = HS_SERVER_HELLO;
    ssl->hs_status = SSL_NOT_OK;            /* not connected */

    /* sit in a loop until it all looks good */
    while (ssl->hs_status != SSL_OK)
    {
        ret = basic_read(ssl, NULL);
        
        if (ret < SSL_OK)
        { 
            if (ret != SSL_ERROR_CONN_LOST)
            {
                /* let the server know we are dying and why */
                if (send_alert(ssl, ret))
                {
                    /* something nasty happened, so get rid of it */
                    kill_ssl_session(ssl->ssl_ctx->ssl_sessions, ssl);
                }
            }

            break;
        }
    }

    ssl->hs_status = ret;            /* connected? */
    return ret;
}

/*
 * Send the initial client hello.
 */
static int send_client_hello(SSL *ssl)
{
    uint8_t *buf = ssl->bm_data;
    time_t tm = time(NULL);
    uint8_t *tm_ptr = &buf[6]; /* time will go here */
    int i, offset;

    buf[0] = HS_CLIENT_HELLO;
    buf[1] = 0;
    buf[2] = 0;
    /* byte 3 is calculated later */
    buf[4] = 0x03;
    buf[5] = 0x01;

    /* client random value - spec says that 1st 4 bytes are big endian time */
    *tm_ptr++ = (uint8_t)(((long)tm & 0xff000000) >> 24);
    *tm_ptr++ = (uint8_t)(((long)tm & 0x00ff0000) >> 16);
    *tm_ptr++ = (uint8_t)(((long)tm & 0x0000ff00) >> 8);
    *tm_ptr++ = (uint8_t)(((long)tm & 0x000000ff));
    get_random(SSL_RANDOM_SIZE-4, &buf[10]);
    memcpy(ssl->client_random, &buf[6], SSL_RANDOM_SIZE);
    offset = 6 + SSL_RANDOM_SIZE;

    /* give session resumption a go */
    if (IS_SET_SSL_FLAG(SSL_SESSION_RESUME))    /* set initially bu user */
    {
        buf[offset++] = SSL_SESSION_ID_SIZE;
        memcpy(&buf[offset], ssl->session_id, SSL_SESSION_ID_SIZE);
        offset += SSL_SESSION_ID_SIZE;
        CLR_SSL_FLAG(SSL_SESSION_RESUME);       /* clear so we can set later */
    }
    else
    {
        /* no session id - because no session resumption just yet */
        buf[offset++] = 0;
    }

    buf[offset++] = 0;              /* number of ciphers */
    buf[offset++] = NUM_PROTOCOLS*2;/* number of ciphers */

    /* put all our supported protocols in our request */
    for (i = 0; i < NUM_PROTOCOLS; i++)
    {
        buf[offset++] = 0;          /* cipher we are using */
        buf[offset++] = ssl_prot_prefs[i];
    }

    buf[offset++] = 1;              /* no compression */
    buf[offset++] = 0;
    buf[3] = offset - 4;            /* handshake size */

    return send_packet(ssl, PT_HANDSHAKE_PROTOCOL, NULL, offset);
}

/*
 * Process the server hello.
 */
static int process_server_hello(SSL *ssl)
{
    uint8_t *buf = ssl->bm_data;
    int pkt_size = ssl->bm_index;
    int offset;
    int version = (buf[4] << 4) + buf[5];
    int num_sessions = ssl->ssl_ctx->num_sessions;
    uint8_t session_id_length;
    int ret = SSL_OK;

    /* check that we are talking to a TLSv1 server */
    if (version != 0x31)
        return SSL_ERROR_INVALID_VERSION;

    /* get the server random value */
    memcpy(ssl->server_random, &buf[6], SSL_RANDOM_SIZE);
    offset = 6 + SSL_RANDOM_SIZE; /* skip of session id size */
    session_id_length = buf[offset++];

    if (num_sessions)
    {
        ssl->session = ssl_session_update(num_sessions,
                ssl->ssl_ctx->ssl_sessions, ssl, &buf[offset]);
        memcpy(ssl->session->session_id, &buf[offset], session_id_length);
    }

    memcpy(ssl->session_id, &buf[offset], session_id_length);
    offset += session_id_length;

    /* get the real cipher we are using */
    ssl->cipher = buf[++offset];
    ssl->next_state = IS_SET_SSL_FLAG(SSL_SESSION_RESUME) ? 
                                        HS_FINISHED : HS_CERTIFICATE;

    PARANOIA_CHECK(pkt_size, offset);

error:
    return ret;
}

/**
 * Process the server hello done message.
 */
static int process_server_hello_done(SSL *ssl)
{
    ssl->next_state = HS_FINISHED;
    return SSL_OK;
}

/*
 * Send a client key exchange message.
 */
static int send_client_key_xchg(SSL *ssl)
{
    uint8_t *buf = ssl->bm_data;
    uint8_t premaster_secret[SSL_SECRET_SIZE];
    int enc_secret_size = -1;

    buf[0] = HS_CLIENT_KEY_XCHG;
    buf[1] = 0;

    premaster_secret[0] = 0x03; /* encode the version number */
    premaster_secret[1] = 0x01;
    get_random(SSL_SECRET_SIZE-2, &premaster_secret[2]);
    DISPLAY_RSA(ssl, "send_client_key_xchg", ssl->x509_ctx->rsa_ctx);

    /* rsa_ctx->bi_ctx is not thread-safe */
    SSL_CTX_LOCK(ssl->ssl_ctx->mutex);
    enc_secret_size = RSA_encrypt(ssl->x509_ctx->rsa_ctx, premaster_secret,
            SSL_SECRET_SIZE, &buf[6], 0);
    SSL_CTX_UNLOCK(ssl->ssl_ctx->mutex);

    buf[2] = (enc_secret_size + 2) >> 8;
    buf[3] = (enc_secret_size + 2) & 0xff;
    buf[4] = enc_secret_size >> 8;
    buf[5] = enc_secret_size & 0xff;

    generate_master_secret(ssl, premaster_secret);
    return send_packet(ssl, PT_HANDSHAKE_PROTOCOL, NULL, enc_secret_size+6);
}

/*
 * Process the certificate request.
 */
static int process_cert_req(SSL *ssl)
{
    /* don't do any processing - we will send back an RSA certificate anyway */
    ssl->next_state = HS_SERVER_HELLO_DONE;
    SET_SSL_FLAG(SSL_HAS_CERT_REQ);
    return SSL_OK;
}

/*
 * Send a certificate verify message.
 */
static int send_cert_verify(SSL *ssl)
{
    uint8_t *buf = ssl->bm_data;
    uint8_t dgst[MD5_SIZE+SHA1_SIZE];
    RSA_CTX *rsa_ctx = ssl->ssl_ctx->rsa_ctx;
    int n = 0, ret;

    DISPLAY_RSA(ssl, "send_cert_verify", rsa_ctx);

    buf[0] = HS_CERT_VERIFY;
    buf[1] = 0;

    finished_digest(ssl, NULL, dgst);   /* calculate the digest */

    /* rsa_ctx->bi_ctx is not thread-safe */
    if (rsa_ctx)
    {
        SSL_CTX_LOCK(ssl->ssl_ctx->mutex);
        n = RSA_encrypt(rsa_ctx, dgst, sizeof(dgst), &buf[6], 1);
        SSL_CTX_UNLOCK(ssl->ssl_ctx->mutex);

        if (n == 0)
        {
            ret = SSL_ERROR_INVALID_KEY;
            goto error;
        }
    }
    
    buf[4] = n >> 8;        /* add the RSA size (not officially documented) */
    buf[5] = n & 0xff;
    n += 2;
    buf[2] = n >> 8;
    buf[3] = n & 0xff;
    ret = send_packet(ssl, PT_HANDSHAKE_PROTOCOL, NULL, n+4);

error:
    return ret;
}

#endif      /* CONFIG_SSL_ENABLE_CLIENT */
