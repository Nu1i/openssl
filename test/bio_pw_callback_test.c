/*
 * Copyright 2024 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include "testutil.h"

#include <openssl/bio.h>
#include <openssl/pem.h>

/* dummy data that needs to be passed to the callback */
typedef struct CallbackData {
    int dummy;
} CALLBACK_DATA;

/* constants */
static char *key_password = "weak_password";
static int key_password_len = 13;
static char *a0a_password = "aaaaaaaa\0aaaaaaaa";
static int a0a_password_len = 17;
static char *a0b_password = "aaaaaaaa\0bbbbbbbb";
static int a0b_password_len = 17;

/* shared working data for all tests */
static char *key_file = NULL;
static EVP_PKEY *original_pkey = NULL;
static BUF_MEM *encrypted_key_data = NULL;
static int encrypted_key_data_size = 0;
static BIO *bio = NULL;
static EVP_PKEY *pkey = NULL;
static CALLBACK_DATA *callback_data = NULL;
static int callback_ret = 0;

/* the test performed by the callback */
typedef enum CallbackTest {
    CB_TEST_NEGATIVE = 0,
    CB_TEST_ZERO_LENGTH,
    CB_TEST_WEAK,
    CB_TEST_16ZERO,
    CB_TEST_A0A,
    CB_TEST_A0B,
    CB_TEST_MATCH_SIZE,
    CB_TEST_EXCEED_SIZE
} CALLBACK_TEST;
static CALLBACK_TEST callback_test = CB_TEST_NEGATIVE;

typedef enum KeyEncoding {
    KE_PEM = 0,
    KE_PKCS8
} KEY_ENCODING;

typedef enum ExpectedResult {
    ER_FAILURE = 0,
    ER_SUCCESS
} EXPECTED_RESULT;

typedef enum OPTION_choice {
    OPT_ERR = -1,
    OPT_EOF = 0,
    OPT_KEY_FILE,
    OPT_TEST_ENUM
} OPTION_CHOICE;

const OPTIONS *test_get_options(void)
{
    static const OPTIONS test_options[] = {
        OPT_TEST_OPTIONS_DEFAULT_USAGE,
        { "keyfile", OPT_KEY_FILE, '<',
          "The PEM file with the encrypted key to load" },
        { NULL }
    };
    return test_options;
}

static void cleanup_after_test(void)
{
    free(encrypted_key_data);
    encrypted_key_data = NULL;
    encrypted_key_data_size = 0;
    BIO_free(bio);
    bio = NULL;
    EVP_PKEY_free(pkey);
    pkey = NULL;
}

static int callback_copy_password(char *buf, int size)
{
    int ret = -1;

    switch (callback_test) {
    case CB_TEST_NEGATIVE:
        break;
    case CB_TEST_ZERO_LENGTH:
        ret = 0;
        break;
    case CB_TEST_WEAK:
        memcpy(buf, key_password, key_password_len);
        ret = key_password_len;
        break;
    case CB_TEST_16ZERO:
        memset(buf, 0, 16);
        ret = 16;
        break;
    case CB_TEST_A0A:
        memcpy(buf, a0a_password, a0a_password_len);
        ret = a0a_password_len;
        break;
    case CB_TEST_A0B:
        memcpy(buf, a0b_password, a0b_password_len);
        ret = a0b_password_len;
        break;
    case CB_TEST_MATCH_SIZE:
        memset(buf, 'e', size);
        ret = size;
        break;
    case CB_TEST_EXCEED_SIZE:
        memset(buf, 'e', size);
        ret = 1000000;
        break;
    }
    return ret;
}

static int read_callback(char *buf, int size, int rwflag, void *u)
{
    int ret = -1;

    /* basic verification of the received data */
    if (!TEST_ptr_eq(u, callback_data))
        goto err;
    if (!TEST_ptr(buf))
        goto err;
    if (!TEST_int_gt(size, 0))
        goto err;
    if (!TEST_int_eq(rwflag, 0))
        goto err;
    ret = callback_copy_password(buf, size);
    callback_ret = 1;
err:
    return ret;
}

static int write_callback(char *buf, int size, int rwflag, void *u)
{
    int ret = -1;

    /* basic verification of the received data */
    if (!TEST_ptr_eq(u, callback_data))
        goto err;
    if (!TEST_ptr(buf))
        goto err;
    if (!TEST_int_gt(size, 0))
        goto err;
    if (!TEST_int_eq(rwflag, 1))
        goto err;
    ret = callback_copy_password(buf, size);
    callback_ret = 1;
err:
    return ret;
}

static int re_encrypt_key(KEY_ENCODING key_encoding)
{
    int w_ret = 0;
    int ret = 0;
    BUF_MEM *bptr = NULL;

    free(encrypted_key_data);
    encrypted_key_data = NULL;
    encrypted_key_data_size = 0;
    if (!TEST_ptr(bio = BIO_new(BIO_s_mem())))
        goto err;
    callback_ret = 0;
    switch (key_encoding) {
    case KE_PEM:
        w_ret = PEM_write_bio_PrivateKey(bio, original_pkey,
                                         EVP_aes_256_cbc(),
                                         NULL, 0, write_callback,
                                         callback_data);
        break;
    case KE_PKCS8:
        w_ret = i2d_PKCS8PrivateKey_bio(bio, original_pkey, EVP_aes_256_cbc(),
                                        NULL, 0, write_callback,
                                        callback_data);
        break;
    }
    if (!TEST_int_ne(w_ret, 0))
        goto err;
    if (!TEST_int_eq(callback_ret, 1))
        goto err;
    encrypted_key_data_size = BIO_get_mem_data(bio, &encrypted_key_data);
    BIO_get_mem_ptr(bio, &bptr);
    if (!BIO_set_close(bio, BIO_NOCLOSE))
        goto err;
    bptr->data = NULL;
    ret = 1;
err:
    BUF_MEM_free(bptr);
    BIO_free(bio);
    bio = NULL;
    return ret;
}

static int decrypt_key(KEY_ENCODING key_encoding,
                       EXPECTED_RESULT expected_result)
{
    EVP_PKEY *r_ret = NULL;
    int ret = 0;

    if (!TEST_ptr(bio = BIO_new_mem_buf(encrypted_key_data,
                                        encrypted_key_data_size)))
        goto err;
    EVP_PKEY_free(pkey);
    pkey = NULL;
    callback_ret = 0;
    switch (key_encoding) {
    case KE_PEM:
        r_ret = PEM_read_bio_PrivateKey(bio, &pkey, read_callback,
                                        callback_data);
        break;
    case KE_PKCS8:
        r_ret = d2i_PKCS8PrivateKey_bio(bio, &pkey, read_callback,
                                        callback_data);
        break;
    }
    if (expected_result == ER_SUCCESS) {
        if (!TEST_ptr(r_ret))
            goto err;
    } else {
        if (!TEST_ptr_null(r_ret))
            goto err;
    }
    if (!TEST_int_eq(callback_ret, 1))
        goto err;
    ret = 1;
err:
    EVP_PKEY_free(pkey);
    pkey = NULL;
    BIO_free(bio);
    bio = NULL;
    return ret;
}

static int full_cycle_test(KEY_ENCODING key_encoding, CALLBACK_TEST write_test,
                           CALLBACK_TEST read_test,
                           EXPECTED_RESULT expected_read_result)
{
    int ret = 0;

    callback_test = write_test;
    callback_ret = 0;
    if (!re_encrypt_key(key_encoding))
        goto err;
    if (!TEST_int_eq(callback_ret, 1))
        goto err;
    callback_test = read_test;
    if (!decrypt_key(key_encoding, expected_read_result))
        goto err;
    if (!TEST_int_eq(callback_ret, 1))
        goto err;
    ret = 1;
err:
    cleanup_after_test();
    return ret;
}

static int test_pem_negative(void)
{
    return full_cycle_test(KE_PEM, CB_TEST_WEAK, CB_TEST_NEGATIVE, ER_FAILURE);
}

static int test_pem_zero_length(void)
{
    return full_cycle_test(KE_PEM, CB_TEST_ZERO_LENGTH, CB_TEST_ZERO_LENGTH,
                           ER_SUCCESS);
}

static int test_pem_weak(void)
{
    return full_cycle_test(KE_PEM, CB_TEST_WEAK, CB_TEST_WEAK, ER_SUCCESS);
}

static int test_pem_16zero(void)
{
    return full_cycle_test(KE_PEM, CB_TEST_16ZERO, CB_TEST_16ZERO, ER_SUCCESS);
}

static int test_pem_a0a(void)
{
    return full_cycle_test(KE_PEM, CB_TEST_A0A, CB_TEST_A0A, ER_SUCCESS);
}

static int test_pem_a0a_a0b(void)
{
    return full_cycle_test(KE_PEM, CB_TEST_A0A, CB_TEST_A0B, ER_FAILURE);
}

static int test_pem_match_size(void)
{
    return full_cycle_test(KE_PEM, CB_TEST_MATCH_SIZE, CB_TEST_MATCH_SIZE,
                           ER_SUCCESS);
}

static int test_pem_exceed_size(void)
{
    return full_cycle_test(KE_PEM, CB_TEST_MATCH_SIZE, CB_TEST_EXCEED_SIZE,
                           ER_FAILURE);
}

static int test_pkcs8_negative(void)
{
    return full_cycle_test(KE_PKCS8, CB_TEST_WEAK, CB_TEST_NEGATIVE, ER_FAILURE);
}

static int test_pkcs8_zero_length(void)
{
    return full_cycle_test(KE_PKCS8, CB_TEST_ZERO_LENGTH, CB_TEST_ZERO_LENGTH,
                           ER_SUCCESS);
}

static int test_pkcs8_weak(void)
{
    return full_cycle_test(KE_PKCS8, CB_TEST_WEAK, CB_TEST_WEAK, ER_SUCCESS);
}

static int test_pkcs8_16zero(void)
{
    return full_cycle_test(KE_PKCS8, CB_TEST_16ZERO, CB_TEST_16ZERO,
                           ER_SUCCESS);
}

static int test_pkcs8_a0a(void)
{
    return full_cycle_test(KE_PKCS8, CB_TEST_A0A, CB_TEST_A0A, ER_SUCCESS);
}

static int test_pkcs8_a0a_a0b(void)
{
    return full_cycle_test(KE_PKCS8, CB_TEST_A0A, CB_TEST_A0B, ER_FAILURE);
}

static int test_pkcs8_match_size(void)
{
    return full_cycle_test(KE_PKCS8, CB_TEST_MATCH_SIZE, CB_TEST_MATCH_SIZE,
                           ER_SUCCESS);
}

static int test_pkcs8_exceed_size(void)
{
    return full_cycle_test(KE_PKCS8, CB_TEST_MATCH_SIZE, CB_TEST_EXCEED_SIZE,
                           ER_FAILURE);
}

static int callback_original_pw(char *buf, int size, int rwflag, void *u)
{
    memcpy(buf, key_password, key_password_len);
    return key_password_len;
}

int setup_tests(void)
{
    OPTION_CHOICE o;

    while ((o = opt_next()) != OPT_EOF) {
        switch (o) {
        case OPT_KEY_FILE:
            key_file = opt_arg();
            break;
        case OPT_TEST_CASES:
            break;
        default:
        case OPT_ERR:
            return 0;
        }
    }

    /* create dummy callback data for verification */
    callback_data = OPENSSL_malloc(sizeof(CALLBACK_DATA));
    memset(callback_data, 0, sizeof(CALLBACK_DATA));

    /* read the original key */
    if (!TEST_ptr(bio = BIO_new_file(key_file, "r")))
        return 0;
    if (!TEST_ptr(PEM_read_bio_PrivateKey(bio, &original_pkey,
                                          callback_original_pw, NULL)))
        return 0;
    BIO_free(bio);
    bio = NULL;

    /* add all tests */
    ADD_TEST(test_pem_negative);
    ADD_TEST(test_pem_zero_length);
    ADD_TEST(test_pem_weak);
    ADD_TEST(test_pem_16zero);
    ADD_TEST(test_pem_a0a);
    ADD_TEST(test_pem_a0a_a0b);
    ADD_TEST(test_pem_match_size);
    ADD_TEST(test_pem_exceed_size);
    ADD_TEST(test_pkcs8_negative);
    ADD_TEST(test_pkcs8_zero_length);
    ADD_TEST(test_pkcs8_weak);
    ADD_TEST(test_pkcs8_16zero);
    ADD_TEST(test_pkcs8_a0a);
    ADD_TEST(test_pkcs8_a0a_a0b);
    ADD_TEST(test_pkcs8_match_size);
    ADD_TEST(test_pkcs8_exceed_size);
    return 1;
}

void cleanup_tests(void)
{
    BUF_MEM_free(encrypted_key_data);
    OPENSSL_free(callback_data);
    EVP_PKEY_free(original_pkey);
}
