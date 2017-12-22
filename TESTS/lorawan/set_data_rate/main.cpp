/* mbed Microcontroller Library
 * Copyright (c) 2017 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "utest/utest.h"
#include "unity/unity.h"
#include "greentea-client/test_env.h"

#include "rtos/Thread.h"
#include "events/EventQueue.h"

#if TARGET_MTS_MDOT_F411RE
#include "SX1272_LoRaRadio.h"
#endif

#if defined(TARGET_K64F) || defined(TARGET_DISCO_L072CZ_LRWAN1)
#include "SX1276_LoRaRadio.h"
#endif
#include "LoRaWANInterface.h"

using namespace utest::v1;
using namespace rtos;
using namespace events;

#ifndef MBED_CONF_LORA_PHY
#error "Must set LoRa PHY layer parameters."
#else
#if MBED_CONF_LORA_PHY      == 0
 #include "LoRaPHYEU868.h"
 static LoRaPHYEU868 lora_phy;
#elif MBED_CONF_LORA_PHY    == 1
#include "LoRaPHYAS923.h"
static LoRaPHYAS923 lora_phy;
#elif MBED_CONF_LORA_PHY    == 2
#include "LoRaPHYAU915.h"
static LoRaPHYAU915 lora_phy;
#elif MBED_CONF_LORA_PHY    == 3
#include "LoRaPHYCN470.h"
static LoRaPHYCN470 lora_phy;
#elif MBED_CONF_LORA_PHY    == 4
#include "LoRaPHYCN779.h"
static LoRaPHYCN779 lora_phy;
#elif MBED_CONF_LORA_PHY    == 5
#include "LoRaPHYEU433.h"
static LoRaPHYEU433 lora_phy;
#elif MBED_CONF_LORA_PHY    == 6
#include "LoRaPHYIN865.h"
static LoRaPHYIN865 lora_phy;
#elif MBED_CONF_LORA_PHY    == 7
#include "LoRaPHYKR920.h"
static LoRaPHYKR920 lora_phy;
#elif MBED_CONF_LORA_PHY    == 8
#include "LoRaPHYUS915.h"
static LoRaPHYUS915 lora_phy;
#elif MBED_CONF_LORA_PHY    == 9
#include "LoRaPHYUS915Hybrid.h"
static LoRaPHYUS915Hybrid lora_phy;
#else
#error "Must set LoRa PHY layer parameters."
#endif
#endif

#ifdef MBED_CONF_APP_TEST_EVENTS_SIZE
 #define MAX_NUMBER_OF_EVENTS    MBED_CONF_APP_TEST_EVENTS_SIZE
#else
 #define MAX_NUMBER_OF_EVENTS   16
#endif

#ifdef MBED_CONF_APP_TEST_DISPATCH_THREAD_SIZE
 #define TEST_DISPATCH_THREAD_SIZE    MBED_CONF_APP_TEST_DISPATCH_THREAD_SIZE
#else
 #define TEST_DISPATCH_THREAD_SIZE    1024
#endif

static EventQueue ev_queue(MAX_NUMBER_OF_EVENTS * EVENTS_EVENT_SIZE);

// This test requires larger stack. Why ?
static Thread t(osPriorityNormal, TEST_DISPATCH_THREAD_SIZE);

void lora_event_handler(lora_events_t events);

#if TARGET_MTS_MDOT_F411RE
    static SX1272_LoRaRadio Radio(LORA_MOSI, LORA_MISO, LORA_SCK, LORA_NSS, LORA_RESET,
                           LORA_DIO0, LORA_DIO1, LORA_DIO2, LORA_DIO3, LORA_DIO4,
                           LORA_DIO5, NC, NC, LORA_TXCTL, LORA_RXCTL, NC, NC);
#endif

#if TARGET_K64F
    static SX1276_LoRaRadio Radio(D11, D12, D13, D10, A0,
                           D2, D3, D4, D5, D8,
                           D9, NC, NC, NC, NC, A4, NC, NC);
#endif

#if defined(TARGET_DISCO_L072CZ_LRWAN1)
    #define LORA_SPI_MOSI   PA_7
    #define LORA_SPI_MISO   PA_6
    #define LORA_SPI_SCLK   PB_3
    #define LORA_CS         PA_15
    #define LORA_RESET      PC_0
    #define LORA_DIO0       PB_4
    #define LORA_DIO1       PB_1
    #define LORA_DIO2       PB_0
    #define LORA_DIO3       PC_13
    #define LORA_DIO4       PA_5
    #define LORA_DIO5       PA_4
    #define LORA_ANT_RX     PA_1
    #define LORA_ANT_TX     PC_2
    #define LORA_ANT_BOOST  PC_1
    #define LORA_TCXO       PA_12   // 32 MHz

    static SX1276_LoRaRadio Radio(LORA_SPI_MOSI, LORA_SPI_MISO, LORA_SPI_SCLK, LORA_CS, LORA_RESET,
                           LORA_DIO0, LORA_DIO1, LORA_DIO2, LORA_DIO3, LORA_DIO4, NC,
                           NC, NC, LORA_ANT_TX, LORA_ANT_RX,
                           NC, LORA_ANT_BOOST, LORA_TCXO);
#endif

class LoRaTestHelper
{
public:
    LoRaTestHelper() :cur_event(0), event_lock(false) {
        memset(event_buffer, 0xFF, sizeof(event_buffer));
    };
    ~LoRaTestHelper() {};

    bool find_event(uint8_t event_code);

    uint8_t event_buffer[10];
    uint8_t cur_event;
    bool event_lock;
};

bool LoRaTestHelper::find_event(uint8_t event_code)
{
    event_lock = true;

    for (uint8_t i = 0; i < 10; i++) {
        if (event_buffer[i] == event_code) {
            event_buffer[i] = 0xFF;
            event_lock = false;
            return true;
        }
    }

    event_lock = false;
    return false;
}

LoRaWANInterface lorawan(Radio);
LoRaTestHelper lora_helper;

void test_data_rate(uint8_t tx_data[11], char *expected_recv_msg)
{
    int16_t ret = 0;
    uint8_t counter = 0;
    uint8_t rx_data[64] = { 0 };

    //Allow upcoming events
    lora_helper.event_lock = false;

    ret = lorawan.send(LORAWAN_APP_PORT, tx_data, sizeof(tx_data), MSG_CONFIRMED_FLAG);

    if (ret != sizeof(tx_data)) {
        TEST_ASSERT_MESSAGE(false, "TX-message buffering failed");
        return;
    }

    while (1) {
        // Wait for TX_DONE event
        if (lora_helper.find_event(TX_DONE)) {
            break;
        }

        // Fail on timeout
        if (counter >= 60) {
            TEST_ASSERT_MESSAGE(false, "Send timeout");
            return;
        }

        wait_ms(1000);
        counter++;
    }

    // Reset timeout counter
    counter = 0;

    while (1) {
        // Wait for RX_DONE event
        if (lora_helper.find_event(RX_DONE)) {
            break;
        }

        // Fail on timeout
        if (counter >= 60) {
            TEST_ASSERT_MESSAGE(false, "Receive timeout");
            return;
        }

        wait_ms(1000);
        counter++;
    }

    ret = lorawan.receive(LORAWAN_APP_PORT, rx_data, 64, MSG_CONFIRMED_FLAG);

    if (ret < 0) {
        TEST_ASSERT_MESSAGE(false, "Receive failed");
        return;
    }

    if (strcmp((char*)rx_data, expected_recv_msg) != 0) {
        TEST_ASSERT_MESSAGE(false, "Incorrect data rate");
        return;
    }

    //Prevent upcoming events between tests
    lora_helper.event_lock = true;
    //Clear the event buffer
    memset(lora_helper.event_buffer, 0xFF, sizeof(lora_helper.event_buffer));

    // Test passed!
}

void lora_set_data_rate()
{
    int16_t ret = 0;
    uint8_t counter = 0;
    uint8_t tx_data[5] = "DR1";
    uint8_t data_rate = 0;

    // ADR must be disabled to set the datarate
    ret = lorawan.disable_adaptive_datarate();

    TEST_ASSERT_MESSAGE(ret == LORA_MAC_STATUS_OK, "Incorrect MAC status");

    ret = lorawan.connect();
    if (ret != LORA_MAC_STATUS_OK && ret != LORA_MAC_STATUS_CONNECT_IN_PROGRESS) {
        TEST_ASSERT_MESSAGE(false, "Connect failed");
        return;
    }

    while (1) {
        if (lora_helper.find_event(CONNECTED)) {
            break;
        }

        // Fail on timeout
        if (counter >= 60) {
            TEST_ASSERT_MESSAGE(false, "Connection timeout");
            return;
        }

        wait_ms(1000);
        counter += 1;
    }

    counter = 0;

    // Set data rate to 1 -> Expected SF11BW125
    data_rate = 1;
    ret = lorawan.set_datarate(data_rate);

    if(ret == LORA_MAC_STATUS_PARAMETER_INVALID) {
        TEST_ASSERT_MESSAGE(false, "Invalid parameter: ADR not disabled or invalid data rate");
    } else if(ret != LORA_MAC_STATUS_OK) {
        TEST_ASSERT_MESSAGE(false, "Incorrect MAC status");
    }

    //Test if data rate is set, expected receive message "SF11BW125"
    test_data_rate(tx_data, "SF11BW125");

    // Set data rate to 3 -> Expected SF9BW125
    data_rate = 3;
    strcpy((char*)tx_data, "DR3");

    ret = lorawan.set_datarate(data_rate);
    if(ret == LORA_MAC_STATUS_PARAMETER_INVALID) {
        TEST_ASSERT_MESSAGE(false, "Invalid parameter: ADR not disabled or invalid data rate");
    } else if(ret != LORA_MAC_STATUS_OK) {
        TEST_ASSERT_MESSAGE(false, "Incorrect MAC status");
    }

    //Test if data rate is set, expected receive message "SF9BW125"
    test_data_rate(tx_data, "SF9BW125");

    // Set data rate to 5 -> Expected SF7BW125
    data_rate = 5;
    strcpy((char*)tx_data, "DR5");
    ret = lorawan.set_datarate(data_rate);

    if(ret == LORA_MAC_STATUS_PARAMETER_INVALID) {
        TEST_ASSERT_MESSAGE(false, "Invalid parameter: ADR not disabled or invalid data rate");
    } else if(ret != LORA_MAC_STATUS_OK) {
        TEST_ASSERT_MESSAGE(false, "Incorrect MAC status");
    }

    //Test if data rate is set, expected receive message "SF7BW125"
    test_data_rate(tx_data, "SF7BW125");

    ret = lorawan.disconnect();
    TEST_ASSERT_MESSAGE(ret == LORA_MAC_STATUS_OK, "Disconnect failed");

    //Test passed
}

utest::v1::status_t greentea_failure_handler(const Case *const source, const failure_t reason) {
    greentea_case_failure_abort_handler(source, reason);
    return STATUS_CONTINUE;
}

Case cases[] = {
    Case("Set data rate", lora_set_data_rate, greentea_failure_handler)
};

utest::v1::status_t greentea_test_setup(const size_t number_of_cases) {
    GREENTEA_SETUP(600, "default_auto");
    return greentea_test_setup_handler(number_of_cases);
}

static lorawan_app_callbacks_t callbacks;

int main() {
    // start thread to handle events
    t.start(callback(&ev_queue, &EventQueue::dispatch_forever));

    lora_mac_status_t ret;

    callbacks.events = mbed::callback(lora_event_handler);

    lorawan.add_app_callbacks(&callbacks);

    ret = lorawan.initialize(&ev_queue);
    TEST_ASSERT_MESSAGE(ret == LORA_MAC_STATUS_OK, "Initialization failed");

    Specification specification(greentea_test_setup, cases, greentea_test_teardown_handler);
    Harness::run(specification);
}

void lora_event_handler(lora_events_t events)
{

    if (lora_helper.event_lock) {
        return;
    }

    lora_helper.event_buffer[lora_helper.cur_event % 10] = events;
    lora_helper.cur_event++;
}