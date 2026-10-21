/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ISC
 */


#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "unity.h"
#include "heatshrink_encoder.h"
#include "heatshrink_decoder.h"
#include "heatshrink_config.h"

void app_main(void)
{
    printf(" /$$                             /$$              /$$                 /$$           /$$\n");
    printf("| $$                            | $$             | $$                |__/          | $$\n");
    printf("| $$$$$$$   /$$$$$$   /$$$$$$  /$$$$$$   /$$$$$$$| $$$$$$$   /$$$$$$  /$$ /$$$$$$$ | $$   /$$\n");
    printf("| $$__  $$ /$$__  $$ |____  $$|_  $$_/  /$$_____/| $$__  $$ /$$__  $$| $$| $$__  $$| $$  /$$/\n");
    printf("| $$  \\ $$| $$$$$$$$  /$$$$$$$  | $$   |  $$$$$$ | $$  \\ $$| $$  \\__/| $$| $$  \\ $$| $$$$$$/\n");
    printf("| $$  | $$| $$_____/ /$$__  $$  | $$ /$$\\____  $$| $$  | $$| $$      | $$| $$  | $$| $$_  $$ \n");
    printf("| $$  | $$|  $$$$$$$|  $$$$$$$  |  $$$$//$$$$$$$/| $$  | $$| $$      | $$| $$  | $$| $$ \\  $$\n");
    printf("|__/  |__/ \\_______/ \\_______/   \\___/ |_______/ |__/  |__/|__/      |__/|__/  |__/|__/  \\__\n");

    unity_run_menu();
}

// Test configuration
#if HEATSHRINK_DYNAMIC_ALLOC
    #define ENCODER_WINDOW_SZ2 8
    #define ENCODER_LOOKAHEAD_SZ2 4
    #define DECODER_INPUT_BUFFER_SIZE 256
    #define DECODER_WINDOW_SZ2 8
    #define DECODER_LOOKAHEAD_SZ2 4
#else
    // Use static configuration from heatshrink_config.h
#endif

// Helper to fill buffer with pseudo-random data
static void fill_random(uint8_t *buf, size_t size, unsigned int seed) {
    srand(seed);
    for (size_t i = 0; i < size; i++) {
        buf[i] = (uint8_t)(rand() & 0xFF);
    }
}

// Helper to Compress and Decompress data
static void test_compress_decompress(uint8_t *input, size_t input_len) {
    // --- ENCODING ---
    size_t sunk_total = 0;
    size_t polled_total = 0;
    size_t comp_sz = input_len * 2 + 64; // Plenty of space
    uint8_t *comp = malloc(comp_sz);
    TEST_ASSERT_NOT_NULL(comp);
    
#if HEATSHRINK_DYNAMIC_ALLOC
    heatshrink_encoder *hse = heatshrink_encoder_alloc(ENCODER_WINDOW_SZ2, ENCODER_LOOKAHEAD_SZ2);
#else
    static heatshrink_encoder hse_static;
    heatshrink_encoder *hse = &hse_static;
    heatshrink_encoder_reset(hse);
#endif
    TEST_ASSERT_NOT_NULL(hse);

    // Sink input with polling
    while (sunk_total < input_len) {
        size_t count = 0;
        HSE_sink_res sres = heatshrink_encoder_sink(hse, &input[sunk_total], input_len - sunk_total, &count);
        TEST_ASSERT_TRUE(sres >= 0);
        sunk_total += count;

        HSE_poll_res pres;
        do {
            size_t out_sz = 0;
            pres = heatshrink_encoder_poll(hse, &comp[polled_total], comp_sz - polled_total, &out_sz);
            TEST_ASSERT_TRUE(pres >= 0);
            polled_total += out_sz;
        } while (pres == HSER_POLL_MORE);
    }

    // Finish
    HSE_finish_res fres = heatshrink_encoder_finish(hse);
    while (fres == HSER_FINISH_MORE) {
        size_t out_sz = 0;
        HSE_poll_res pres = heatshrink_encoder_poll(hse, &comp[polled_total], comp_sz - polled_total, &out_sz);
        TEST_ASSERT_TRUE(pres >= 0);
        polled_total += out_sz;
        fres = heatshrink_encoder_finish(hse);
    }
    TEST_ASSERT_EQUAL(HSER_FINISH_DONE, fres);

#if HEATSHRINK_DYNAMIC_ALLOC
    heatshrink_encoder_free(hse);
#endif

    // --- DECODING ---
    size_t decomp_sz = input_len + 64;
    uint8_t *decomp = malloc(decomp_sz);
    TEST_ASSERT_NOT_NULL(decomp);

#if HEATSHRINK_DYNAMIC_ALLOC
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(DECODER_INPUT_BUFFER_SIZE, DECODER_WINDOW_SZ2, DECODER_LOOKAHEAD_SZ2);
#else
    static heatshrink_decoder hsd_static;
    heatshrink_decoder *hsd = &hsd_static;
    heatshrink_decoder_reset(hsd);
#endif
    TEST_ASSERT_NOT_NULL(hsd);

    size_t decoded_sunk_total = 0;
    size_t decoded_polled_total = 0;
    
    // Sink compressed data with polling
    while (decoded_sunk_total < polled_total) {
        size_t sink_sz = 0;
        HSD_sink_res d_sres = heatshrink_decoder_sink(hsd, &comp[decoded_sunk_total], polled_total - decoded_sunk_total, &sink_sz);
        TEST_ASSERT_TRUE(d_sres >= 0);
        decoded_sunk_total += sink_sz;

        HSD_poll_res d_pres;
        do {
            size_t out_sz = 0;
            d_pres = heatshrink_decoder_poll(hsd, &decomp[decoded_polled_total], decomp_sz - decoded_polled_total, &out_sz);
            TEST_ASSERT_TRUE(d_pres >= 0);
            decoded_polled_total += out_sz;
        } while (d_pres == HSDR_POLL_MORE);
    }
    
    HSD_finish_res d_fres = heatshrink_decoder_finish(hsd);
     while (d_fres == HSDR_FINISH_MORE) {
         size_t out_sz = 0;
         HSD_poll_res d_pres = heatshrink_decoder_poll(hsd, &decomp[decoded_polled_total], decomp_sz - decoded_polled_total, &out_sz);
         TEST_ASSERT_TRUE(d_pres >= 0);
         decoded_polled_total += out_sz;
         d_fres = heatshrink_decoder_finish(hsd);
     }
    TEST_ASSERT_EQUAL(HSDR_FINISH_DONE, d_fres);

    // Verify
    TEST_ASSERT_EQUAL(input_len, decoded_polled_total);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(input, decomp, input_len);

#if HEATSHRINK_DYNAMIC_ALLOC
    heatshrink_decoder_free(hsd);
#endif
    free(comp);
    free(decomp);
}

TEST_CASE("Heatshrink Basic Round Trip", "[heatshrink]")
{
    char *input = "The quick brown fox jumps over the lazy dog";
    test_compress_decompress((uint8_t*)input, strlen(input) + 1); // include null terminator
}

TEST_CASE("Heatshrink Pattern Repetition", "[heatshrink]")
{
    size_t len = 512;
    uint8_t *input = malloc(len);
    TEST_ASSERT_NOT_NULL(input);
    for (size_t i = 0; i < len; i++) {
        input[i] = (uint8_t)(i % 16); // Repeating pattern
    }
    test_compress_decompress(input, len);
    free(input);
}

TEST_CASE("Heatshrink Random Data", "[heatshrink]")
{
    size_t len = 1024; // 1KB Random
    uint8_t *input = malloc(len);
    TEST_ASSERT_NOT_NULL(input);
    fill_random(input, len, 12345);
    test_compress_decompress(input, len);
    free(input);
}

TEST_CASE("Heatshrink Small Input", "[heatshrink]")
{
    uint8_t input[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    test_compress_decompress(input, sizeof(input));
}

TEST_CASE("Heatshrink Exact Buffer Size", "[heatshrink]")
{
    // Test boundary conditions where buffer sizes match exactly
     char *input = "12345678"; // 8 bytes (9 with null)
     test_compress_decompress((uint8_t*)input, 9);
}
