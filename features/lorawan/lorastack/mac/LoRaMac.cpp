/**
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
    (C)2013 Semtech
 ___ _____ _   ___ _  _____ ___  ___  ___ ___
/ __|_   _/_\ / __| |/ / __/ _ \| _ \/ __| __|
\__ \ | |/ _ \ (__| ' <| _| (_) |   / (__| _|
|___/ |_/_/ \_\___|_|\_\_| \___/|_|_\\___|___|
embedded.connectivity.solutions===============

Description: LoRa MAC layer implementation

License: Revised BSD License, see LICENSE.TXT file include in the project

Maintainer: Miguel Luis ( Semtech ), Gregory Cristian ( Semtech ) and Daniel Jaeckle ( STACKFORCE )

Copyright (c) 2017, Arm Limited and affiliates.

SPDX-License-Identifier: BSD-3-Clause
*/
#include <stdlib.h>
#include "LoRaMac.h"
#include "LoRaMacCrypto.h"

#if defined(FEATURE_COMMON_PAL)
#include "mbed_trace.h"
#define TRACE_GROUP "LMAC"
#else
#define tr_debug(...) (void(0)) //dummies if feature common pal is not added
#define tr_info(...)  (void(0)) //dummies if feature common pal is not added
#define tr_error(...) (void(0)) //dummies if feature common pal is not added
#endif //defined(FEATURE_COMMON_PAL)

using namespace events;

/**
 * EventQueue object storage
 */
static EventQueue *ev_queue;

/*!
 * Maximum length of the fOpts field
 */
#define LORA_MAC_COMMAND_MAX_FOPTS_LENGTH           15

/*!
 * LoRaMac duty cycle for the back-off procedure during the first hour.
 */
#define BACKOFF_DC_1_HOUR                           100

/*!
 * LoRaMac duty cycle for the back-off procedure during the next 10 hours.
 */
#define BACKOFF_DC_10_HOURS                         1000

/*!
 * LoRaMac duty cycle for the back-off procedure during the next 24 hours.
 */
#define BACKOFF_DC_24_HOURS                         10000

/*!
 * Check the MAC layer state every MAC_STATE_CHECK_TIMEOUT in ms.
 */
#define MAC_STATE_CHECK_TIMEOUT                     1000

/*!
 * The maximum number of times the MAC layer tries to get an acknowledge.
 */
#define MAX_ACK_RETRIES                             8

/*!
 * The frame direction definition for uplink communications.
 */
#define UP_LINK                                     0

/*!
 * The frame direction definition for downlink communications.
 */
#define DOWN_LINK                                   1


LoRaMac::LoRaMac(LoRaWANTimeHandler &lora_time)
    : mac_commands(*this),
      _lora_time(lora_time)
{
    lora_phy = NULL;
    //radio_events_t RadioEvents;
    LoRaMacDevEui = NULL;
    LoRaMacAppEui = NULL;
    LoRaMacAppKey = NULL;

    memset(LoRaMacNwkSKey, 0, sizeof(LoRaMacNwkSKey));
    memset(LoRaMacAppSKey, 0, sizeof(LoRaMacAppSKey));

    LoRaMacDevNonce = 0;
    LoRaMacNetID = 0;
    LoRaMacDevAddr = 0;
    MulticastChannels = NULL;
    //DeviceClass_t LoRaMacDeviceClass;
    //bool PublicNetwork;
    //bool RepeaterSupport;
    //uint8_t LoRaMacBuffer[LORAMAC_PHY_MAXPAYLOAD];
    LoRaMacBufferPktLen = 0;
    LoRaMacTxPayloadLen = 0;
    //uint8_t LoRaMacRxPayload[LORAMAC_PHY_MAXPAYLOAD];
    UpLinkCounter = 0;
    DownLinkCounter = 0;
    IsUpLinkCounterFixed = false;
    IsRxWindowsEnabled = true;
    IsLoRaMacNetworkJoined = false;
    LoRaMacParams.AdrCtrlOn = false;
    AdrAckCounter = 0;
    NodeAckRequested = false;
    SrvAckRequested = false;
    //LoRaMacParams_t LoRaMacParams;
    //LoRaMacParams_t LoRaMacParamsDefaults;
    ChannelsNbRepCounter = 0;
    LoRaMacParams.MaxDCycle = 0;
    //uint16_t AggregatedDCycle;
    //TimerTime_t AggregatedLastTxDoneTime;
    //TimerTime_t AggregatedTimeOff;
    //bool DutyCycleOn;
    //uint8_t Channel;
    //uint8_t LastTxChannel;
    //bool LastTxIsJoinRequest;
    LoRaMacInitializationTime = 0;
    LoRaMacState = LORAMAC_IDLE;
    //TimerEvent_t MacStateCheckTimer;
    //TimerEvent_t TxNextPacketTimer;
    //LoRaMacPrimitives_t *LoRaMacPrimitives;
    //LoRaMacCallback_t *LoRaMacCallbacks;
    //TimerEvent_t TxDelayedTimer;
    //TimerEvent_t RxWindowTimer1;
    //TimerEvent_t RxWindowTimer2;
    //uint32_t RxWindow1Delay;
    //uint32_t RxWindow2Delay;
    //RxConfigParams_t RxWindow1Config;
    //RxConfigParams_t RxWindow2Config;
    //TimerEvent_t AckTimeoutTimer;
    AckTimeoutRetries = 1;
    AckTimeoutRetriesCounter = 1;
    AckTimeoutRetry = false;
    TxTimeOnAir = 0;
    //uint8_t JoinRequestTrials;
    //uint8_t MaxJoinRequestTrials;
    //McpsIndication_t McpsIndication;
    //McpsConfirm_t McpsConfirm;
    //MlmeConfirm_t MlmeConfirm;
    //LoRaMacRxSlot_t RxSlot;
    //LoRaMacFlags_t LoRaMacFlags;
}

LoRaMac::~LoRaMac()
{
}


/***************************************************************************
 * ISRs - Handlers                                                         *
 **************************************************************************/
void LoRaMac::handle_tx_done(void)
{
    ev_queue->call(this, &LoRaMac::OnRadioTxDone);
}

void LoRaMac::handle_rx_done(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr)
{
    ev_queue->call(this, &LoRaMac::OnRadioRxDone, payload, size, rssi, snr);
}

void LoRaMac::handle_rx_error(void)
{
    ev_queue->call(this, &LoRaMac::OnRadioRxError);
}

void LoRaMac::handle_rx_timeout(void)
{
    ev_queue->call(this, &LoRaMac::OnRadioRxTimeout);
}

void LoRaMac::handle_tx_timeout(void)
{
    ev_queue->call(this, &LoRaMac::OnRadioTxTimeout);
}

void LoRaMac::handle_cad_done(bool cad)
{
    //TODO Not implemented yet
    //ev_queue->call(this, &LoRaMac::OnRadioCadDone, cad);
}

void LoRaMac::handle_fhss_change_channel(uint8_t cur_channel)
{
    // TODO Not implemented yet
    //ev_queue->call(this, &LoRaMac::OnRadioFHSSChangeChannel, cur_channel);
}

void LoRaMac::handle_mac_state_check_timer_event(void)
{
    ev_queue->call(this, &LoRaMac::OnMacStateCheckTimerEvent);
}

void LoRaMac::handle_delayed_tx_timer_event(void)
{
    ev_queue->call(this, &LoRaMac::OnTxDelayedTimerEvent);
}

void LoRaMac::handle_ack_timeout()
{
    ev_queue->call(this, &LoRaMac::OnAckTimeoutTimerEvent);
}

void LoRaMac::handle_rx1_timer_event(void)
{
    ev_queue->call(this, &LoRaMac::OnRxWindow1TimerEvent);
}

void LoRaMac::handle_rx2_timer_event(void)
{
    ev_queue->call(this, &LoRaMac::OnRxWindow2TimerEvent);
}

/***************************************************************************
 * Radio event callbacks - delegated to Radio driver                       *
 **************************************************************************/
void LoRaMac::OnRadioTxDone( void )
{
    GetPhyParams_t getPhy;
    PhyParam_t phyParam;
    SetBandTxDoneParams_t txDone;
    TimerTime_t curTime = _lora_time.TimerGetCurrentTime( );

    if( LoRaMacDeviceClass != CLASS_C )
    {
        lora_phy->put_radio_to_sleep();
    }
    else
    {
        OpenContinuousRx2Window( );
    }

    // Setup timers
    if( IsRxWindowsEnabled == true )
    {
        _lora_time.TimerSetValue( &RxWindowTimer1, RxWindow1Delay );
        _lora_time.TimerStart( &RxWindowTimer1 );
        if( LoRaMacDeviceClass != CLASS_C )
        {
            _lora_time.TimerSetValue( &RxWindowTimer2, RxWindow2Delay );
            _lora_time.TimerStart( &RxWindowTimer2 );
        }
        if( ( LoRaMacDeviceClass == CLASS_C ) || ( NodeAckRequested == true ) )
        {
            getPhy.Attribute = PHY_ACK_TIMEOUT;
            phyParam = lora_phy->get_phy_params(&getPhy);
            _lora_time.TimerSetValue( &AckTimeoutTimer, RxWindow2Delay + phyParam.Value );
            _lora_time.TimerStart( &AckTimeoutTimer );
        }
    }
    else
    {
        McpsConfirm.Status = LORAMAC_EVENT_INFO_STATUS_OK;
        MlmeConfirm.Status = LORAMAC_EVENT_INFO_STATUS_RX2_TIMEOUT;

        if( LoRaMacFlags.Value == 0 )
        {
            LoRaMacFlags.Bits.McpsReq = 1;
        }
        LoRaMacFlags.Bits.MacDone = 1;
    }

    // Verify if the last uplink was a join request
    if( ( LoRaMacFlags.Bits.MlmeReq == 1 ) && ( MlmeConfirm.MlmeRequest == MLME_JOIN ) )
    {
        LastTxIsJoinRequest = true;
    }
    else
    {
        LastTxIsJoinRequest = false;
    }

    // Store last Tx channel
    LastTxChannel = Channel;
    // Update last tx done time for the current channel
    txDone.Channel = Channel;
    txDone.Joined = IsLoRaMacNetworkJoined;
    txDone.LastTxDoneTime = curTime;
    lora_phy->set_band_tx_done(&txDone);
    // Update Aggregated last tx done time
    AggregatedLastTxDoneTime = curTime;

    if( NodeAckRequested == false )
    {
        McpsConfirm.Status = LORAMAC_EVENT_INFO_STATUS_OK;
        ChannelsNbRepCounter++;
    }
}

void LoRaMac::PrepareRxDoneAbort( void )
{
    LoRaMacState |= LORAMAC_RX_ABORT;

    if( NodeAckRequested )
    {
        handle_ack_timeout();
    }

    LoRaMacFlags.Bits.McpsInd = 1;
    LoRaMacFlags.Bits.MacDone = 1;

    // Trig OnMacCheckTimerEvent call as soon as possible
    _lora_time.TimerSetValue( &MacStateCheckTimer, 1 );
    _lora_time.TimerStart( &MacStateCheckTimer );
}

void LoRaMac::OnRadioRxDone( uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr )
{
    LoRaMacHeader_t macHdr;
    LoRaMacFrameCtrl_t fCtrl;
    ApplyCFListParams_t applyCFList;
    GetPhyParams_t getPhy;
    PhyParam_t phyParam;
    bool skipIndication = false;

    uint8_t pktHeaderLen = 0;
    uint32_t address = 0;
    uint8_t appPayloadStartIndex = 0;
    uint8_t port = 0xFF;
    uint8_t frameLen = 0;
    uint32_t mic = 0;
    uint32_t micRx = 0;

    uint16_t sequenceCounter = 0;
    uint16_t sequenceCounterPrev = 0;
    uint16_t sequenceCounterDiff = 0;
    uint32_t downLinkCounter = 0;

    MulticastParams_t *curMulticastParams = NULL;
    uint8_t *nwkSKey = LoRaMacNwkSKey;
    uint8_t *appSKey = LoRaMacAppSKey;

    uint8_t multicast = 0;

    bool isMicOk = false;

    McpsConfirm.AckReceived = false;
    McpsIndication.Rssi = rssi;
    McpsIndication.Snr = snr;
    McpsIndication.RxSlot = RxSlot;
    McpsIndication.Port = 0;
    McpsIndication.Multicast = 0;
    McpsIndication.FramePending = 0;
    McpsIndication.Buffer = NULL;
    McpsIndication.BufferSize = 0;
    McpsIndication.RxData = false;
    McpsIndication.AckReceived = false;
    McpsIndication.DownLinkCounter = 0;
    McpsIndication.McpsIndication = MCPS_UNCONFIRMED;

    lora_phy->put_radio_to_sleep();

    _lora_time.TimerStop( &RxWindowTimer2 );

    macHdr.Value = payload[pktHeaderLen++];

    switch( macHdr.Bits.MType )
    {
        case FRAME_TYPE_JOIN_ACCEPT:
            if( IsLoRaMacNetworkJoined == true )
            {
                McpsIndication.Status = LORAMAC_EVENT_INFO_STATUS_ERROR;
                PrepareRxDoneAbort( );
                return;
            }

            if (0 != LoRaMacJoinDecrypt( payload + 1, size - 1, LoRaMacAppKey, LoRaMacRxPayload + 1 )) {
                McpsIndication.Status = LORAMAC_EVENT_INFO_STATUS_CRYPTO_FAIL;
                return;
            }

            LoRaMacRxPayload[0] = macHdr.Value;

            if (0 != LoRaMacJoinComputeMic( LoRaMacRxPayload, size - LORAMAC_MFR_LEN, LoRaMacAppKey, &mic )) {
                McpsIndication.Status = LORAMAC_EVENT_INFO_STATUS_CRYPTO_FAIL;
                return;
            }

            micRx |= ( uint32_t )LoRaMacRxPayload[size - LORAMAC_MFR_LEN];
            micRx |= ( ( uint32_t )LoRaMacRxPayload[size - LORAMAC_MFR_LEN + 1] << 8 );
            micRx |= ( ( uint32_t )LoRaMacRxPayload[size - LORAMAC_MFR_LEN + 2] << 16 );
            micRx |= ( ( uint32_t )LoRaMacRxPayload[size - LORAMAC_MFR_LEN + 3] << 24 );

            if( micRx == mic )
            {
                if (0 != LoRaMacJoinComputeSKeys( LoRaMacAppKey, LoRaMacRxPayload + 1, LoRaMacDevNonce, LoRaMacNwkSKey, LoRaMacAppSKey )) {
                    McpsIndication.Status = LORAMAC_EVENT_INFO_STATUS_CRYPTO_FAIL;
                    return;
                }

                LoRaMacNetID = ( uint32_t )LoRaMacRxPayload[4];
                LoRaMacNetID |= ( ( uint32_t )LoRaMacRxPayload[5] << 8 );
                LoRaMacNetID |= ( ( uint32_t )LoRaMacRxPayload[6] << 16 );

                LoRaMacDevAddr = ( uint32_t )LoRaMacRxPayload[7];
                LoRaMacDevAddr |= ( ( uint32_t )LoRaMacRxPayload[8] << 8 );
                LoRaMacDevAddr |= ( ( uint32_t )LoRaMacRxPayload[9] << 16 );
                LoRaMacDevAddr |= ( ( uint32_t )LoRaMacRxPayload[10] << 24 );

                // DLSettings
                LoRaMacParams.Rx1DrOffset = ( LoRaMacRxPayload[11] >> 4 ) & 0x07;
                LoRaMacParams.Rx2Channel.Datarate = LoRaMacRxPayload[11] & 0x0F;

                // RxDelay
                LoRaMacParams.ReceiveDelay1 = ( LoRaMacRxPayload[12] & 0x0F );
                if( LoRaMacParams.ReceiveDelay1 == 0 )
                {
                    LoRaMacParams.ReceiveDelay1 = 1;
                }
                LoRaMacParams.ReceiveDelay1 *= 1000;
                LoRaMacParams.ReceiveDelay2 = LoRaMacParams.ReceiveDelay1 + 1000;

                // Apply CF list
                applyCFList.Payload = &LoRaMacRxPayload[13];
                // Size of the regular payload is 12. Plus 1 byte MHDR and 4 bytes MIC
                applyCFList.Size = size - 17;

                lora_phy->apply_cf_list(&applyCFList);

                MlmeConfirm.Status = LORAMAC_EVENT_INFO_STATUS_OK;
                IsLoRaMacNetworkJoined = true;
            }
            else
            {
                MlmeConfirm.Status = LORAMAC_EVENT_INFO_STATUS_JOIN_FAIL;
            }
            break;
        case FRAME_TYPE_DATA_CONFIRMED_DOWN:
        case FRAME_TYPE_DATA_UNCONFIRMED_DOWN:
            {
                // Check if the received payload size is valid
                getPhy.UplinkDwellTime = LoRaMacParams.DownlinkDwellTime;
                getPhy.Datarate = McpsIndication.RxDatarate;
                getPhy.Attribute = PHY_MAX_PAYLOAD;

                // Get the maximum payload length
                if( RepeaterSupport == true )
                {
                    getPhy.Attribute = PHY_MAX_PAYLOAD_REPEATER;
                }
                phyParam = lora_phy->get_phy_params(&getPhy);
                if( MAX( 0, ( int16_t )( ( int16_t )size - ( int16_t )LORA_MAC_FRMPAYLOAD_OVERHEAD ) ) > phyParam.Value )
                {
                    McpsIndication.Status = LORAMAC_EVENT_INFO_STATUS_ERROR;
                    PrepareRxDoneAbort( );
                    return;
                }

                address = payload[pktHeaderLen++];
                address |= ( (uint32_t)payload[pktHeaderLen++] << 8 );
                address |= ( (uint32_t)payload[pktHeaderLen++] << 16 );
                address |= ( (uint32_t)payload[pktHeaderLen++] << 24 );

                if( address != LoRaMacDevAddr )
                {
                    curMulticastParams = MulticastChannels;
                    while( curMulticastParams != NULL )
                    {
                        if( address == curMulticastParams->Address )
                        {
                            multicast = 1;
                            nwkSKey = curMulticastParams->NwkSKey;
                            appSKey = curMulticastParams->AppSKey;
                            downLinkCounter = curMulticastParams->DownLinkCounter;
                            break;
                        }
                        curMulticastParams = curMulticastParams->Next;
                    }
                    if( multicast == 0 )
                    {
                        // We are not the destination of this frame.
                        McpsIndication.Status = LORAMAC_EVENT_INFO_STATUS_ADDRESS_FAIL;
                        PrepareRxDoneAbort( );
                        return;
                    }
                }
                else
                {
                    multicast = 0;
                    nwkSKey = LoRaMacNwkSKey;
                    appSKey = LoRaMacAppSKey;
                    downLinkCounter = DownLinkCounter;
                }

                fCtrl.Value = payload[pktHeaderLen++];

                sequenceCounter = ( uint16_t )payload[pktHeaderLen++];
                sequenceCounter |= ( uint16_t )payload[pktHeaderLen++] << 8;

                appPayloadStartIndex = 8 + fCtrl.Bits.FOptsLen;

                micRx |= ( uint32_t )payload[size - LORAMAC_MFR_LEN];
                micRx |= ( ( uint32_t )payload[size - LORAMAC_MFR_LEN + 1] << 8 );
                micRx |= ( ( uint32_t )payload[size - LORAMAC_MFR_LEN + 2] << 16 );
                micRx |= ( ( uint32_t )payload[size - LORAMAC_MFR_LEN + 3] << 24 );

                sequenceCounterPrev = ( uint16_t )downLinkCounter;
                sequenceCounterDiff = ( sequenceCounter - sequenceCounterPrev );

                if( sequenceCounterDiff < ( 1 << 15 ) )
                {
                    downLinkCounter += sequenceCounterDiff;
                    LoRaMacComputeMic( payload, size - LORAMAC_MFR_LEN, nwkSKey, address, DOWN_LINK, downLinkCounter, &mic );
                    if( micRx == mic )
                    {
                        isMicOk = true;
                    }
                }
                else
                {
                    // check for sequence roll-over
                    uint32_t  downLinkCounterTmp = downLinkCounter + 0x10000 + ( int16_t )sequenceCounterDiff;
                    LoRaMacComputeMic( payload, size - LORAMAC_MFR_LEN, nwkSKey, address, DOWN_LINK, downLinkCounterTmp, &mic );
                    if( micRx == mic )
                    {
                        isMicOk = true;
                        downLinkCounter = downLinkCounterTmp;
                    }
                }

                // Check for a the maximum allowed counter difference
                getPhy.Attribute = PHY_MAX_FCNT_GAP;
                phyParam = lora_phy->get_phy_params( &getPhy );
                if( sequenceCounterDiff >= phyParam.Value )
                {
                    McpsIndication.Status = LORAMAC_EVENT_INFO_STATUS_DOWNLINK_TOO_MANY_FRAMES_LOSS;
                    McpsIndication.DownLinkCounter = downLinkCounter;
                    PrepareRxDoneAbort( );
                    return;
                }

                if( isMicOk == true )
                {
                    McpsIndication.Status = LORAMAC_EVENT_INFO_STATUS_OK;
                    McpsIndication.Multicast = multicast;
                    McpsIndication.FramePending = fCtrl.Bits.FPending;
                    McpsIndication.Buffer = NULL;
                    McpsIndication.BufferSize = 0;
                    McpsIndication.DownLinkCounter = downLinkCounter;

                    McpsConfirm.Status = LORAMAC_EVENT_INFO_STATUS_OK;

                    AdrAckCounter = 0;
                    mac_commands.ClearRepeatBuffer();

                    // Update 32 bits downlink counter
                    if( multicast == 1 )
                    {
                        McpsIndication.McpsIndication = MCPS_MULTICAST;

                        if( ( curMulticastParams->DownLinkCounter == downLinkCounter ) &&
                            ( curMulticastParams->DownLinkCounter != 0 ) )
                        {
                            McpsIndication.Status = LORAMAC_EVENT_INFO_STATUS_DOWNLINK_REPEATED;
                            McpsIndication.DownLinkCounter = downLinkCounter;
                            PrepareRxDoneAbort( );
                            return;
                        }
                        curMulticastParams->DownLinkCounter = downLinkCounter;
                    }
                    else
                    {
                        if( macHdr.Bits.MType == FRAME_TYPE_DATA_CONFIRMED_DOWN )
                        {
                            SrvAckRequested = true;
                            McpsIndication.McpsIndication = MCPS_CONFIRMED;

                            if( ( DownLinkCounter == downLinkCounter ) &&
                                ( DownLinkCounter != 0 ) )
                            {
                                // Duplicated confirmed downlink. Skip indication.
                                // In this case, the MAC layer shall accept the MAC commands
                                // which are included in the downlink retransmission.
                                // It should not provide the same frame to the application
                                // layer again.
                                skipIndication = true;
                            }
                        }
                        else
                        {
                            SrvAckRequested = false;
                            McpsIndication.McpsIndication = MCPS_UNCONFIRMED;

                            if( ( DownLinkCounter == downLinkCounter ) &&
                                ( DownLinkCounter != 0 ) )
                            {
                                McpsIndication.Status = LORAMAC_EVENT_INFO_STATUS_DOWNLINK_REPEATED;
                                McpsIndication.DownLinkCounter = downLinkCounter;
                                PrepareRxDoneAbort( );
                                return;
                            }
                        }
                        DownLinkCounter = downLinkCounter;
                    }

                    // This must be done before parsing the payload and the MAC commands.
                    // We need to reset the MacCommandsBufferIndex here, since we need
                    // to take retransmissions and repetitions into account. Error cases
                    // will be handled in function OnMacStateCheckTimerEvent.
                    if( McpsConfirm.McpsRequest == MCPS_CONFIRMED )
                    {
                        if( fCtrl.Bits.Ack == 1 )
                        {// Reset MacCommandsBufferIndex when we have received an ACK.
                            mac_commands.ClearCommandBuffer();
                        }
                    }
                    else
                    {// Reset the variable if we have received any valid frame.
                        mac_commands.ClearCommandBuffer();
                    }

                    // Process payload and MAC commands
                    if( ( ( size - 4 ) - appPayloadStartIndex ) > 0 )
                    {
                        port = payload[appPayloadStartIndex++];
                        frameLen = ( size - 4 ) - appPayloadStartIndex;

                        McpsIndication.Port = port;

                        if( port == 0 )
                        {
                            // Only allow frames which do not have fOpts
                            if( fCtrl.Bits.FOptsLen == 0 )
                            {
                                if (0 != LoRaMacPayloadDecrypt( payload + appPayloadStartIndex,
                                                                frameLen,
                                                                nwkSKey,
                                                                address,
                                                                DOWN_LINK,
                                                                downLinkCounter,
                                                                LoRaMacRxPayload )) {
                                    McpsIndication.Status =  LORAMAC_EVENT_INFO_STATUS_CRYPTO_FAIL;
                                }

                                // Decode frame payload MAC commands
                                mac_commands.ProcessMacCommands( LoRaMacRxPayload, 0, frameLen, snr,
                                                                 MlmeConfirm, LoRaMacCallbacks,
                                                                 LoRaMacParams, *lora_phy );
                            }
                            else
                            {
                                skipIndication = true;
                            }
                        }
                        else
                        {
                            if( fCtrl.Bits.FOptsLen > 0 )
                            {
                                // Decode Options field MAC commands. Omit the fPort.
                                mac_commands.ProcessMacCommands( payload, 8, appPayloadStartIndex - 1, snr,
                                                                 MlmeConfirm, LoRaMacCallbacks,
                                                                 LoRaMacParams, *lora_phy );
                            }

                            if (0 != LoRaMacPayloadDecrypt( payload + appPayloadStartIndex,
                                                            frameLen,
                                                            appSKey,
                                                            address,
                                                            DOWN_LINK,
                                                            downLinkCounter,
                                                            LoRaMacRxPayload )) {
                                McpsIndication.Status =  LORAMAC_EVENT_INFO_STATUS_CRYPTO_FAIL;
                            }

                            if( skipIndication == false )
                            {
                                McpsIndication.Buffer = LoRaMacRxPayload;
                                McpsIndication.BufferSize = frameLen;
                                McpsIndication.RxData = true;
                            }
                        }
                    }
                    else
                    {
                        if( fCtrl.Bits.FOptsLen > 0 )
                        {
                            // Decode Options field MAC commands
                            mac_commands.ProcessMacCommands( payload, 8, appPayloadStartIndex, snr,
                                                             MlmeConfirm, LoRaMacCallbacks,
                                                             LoRaMacParams, *lora_phy );
                        }
                    }

                    if( skipIndication == false )
                    {
                        // Check if the frame is an acknowledgement
                        if( fCtrl.Bits.Ack == 1 )
                        {
                            McpsConfirm.AckReceived = true;
                            McpsIndication.AckReceived = true;

                            // Stop the AckTimeout timer as no more retransmissions
                            // are needed.
                            _lora_time.TimerStop( &AckTimeoutTimer );
                        }
                        else
                        {
                            McpsConfirm.AckReceived = false;

                            if( AckTimeoutRetriesCounter > AckTimeoutRetries )
                            {
                                // Stop the AckTimeout timer as no more retransmissions
                                // are needed.
                                _lora_time.TimerStop( &AckTimeoutTimer );
                            }
                        }
                    }
                    // Provide always an indication, skip the callback to the user application,
                    // in case of a confirmed downlink retransmission.
                    LoRaMacFlags.Bits.McpsInd = 1;
                    LoRaMacFlags.Bits.McpsIndSkip = skipIndication;
                }
                else
                {
                    McpsIndication.Status = LORAMAC_EVENT_INFO_STATUS_MIC_FAIL;

                    PrepareRxDoneAbort( );
                    return;
                }
            }
            break;
        case FRAME_TYPE_PROPRIETARY:
            {
                memcpy( LoRaMacRxPayload, &payload[pktHeaderLen], size );

                McpsIndication.McpsIndication = MCPS_PROPRIETARY;
                McpsIndication.Status = LORAMAC_EVENT_INFO_STATUS_OK;
                McpsIndication.Buffer = LoRaMacRxPayload;
                McpsIndication.BufferSize = size - pktHeaderLen;

                LoRaMacFlags.Bits.McpsInd = 1;
                break;
            }
        default:
            McpsIndication.Status = LORAMAC_EVENT_INFO_STATUS_ERROR;
            PrepareRxDoneAbort( );
            break;
    }
    LoRaMacFlags.Bits.MacDone = 1;

    // Trig OnMacCheckTimerEvent call as soon as possible
    _lora_time.TimerSetValue( &MacStateCheckTimer, 1 );
    _lora_time.TimerStart( &MacStateCheckTimer );
}

void LoRaMac::OnRadioTxTimeout( void )
{
    if( LoRaMacDeviceClass != CLASS_C )
    {
        lora_phy->put_radio_to_sleep();
    }
    else
    {
        OpenContinuousRx2Window( );
    }

    McpsConfirm.Status = LORAMAC_EVENT_INFO_STATUS_TX_TIMEOUT;
    MlmeConfirm.Status = LORAMAC_EVENT_INFO_STATUS_TX_TIMEOUT;
    LoRaMacFlags.Bits.MacDone = 1;
}

void LoRaMac::OnRadioRxError( void )
{
    if( LoRaMacDeviceClass != CLASS_C )
    {
        lora_phy->put_radio_to_sleep();
    }
    else
    {
        OpenContinuousRx2Window( );
    }

    if( RxSlot == RX_SLOT_WIN_1 )
    {
        if( NodeAckRequested == true )
        {
            McpsConfirm.Status = LORAMAC_EVENT_INFO_STATUS_RX1_ERROR;
        }
        MlmeConfirm.Status = LORAMAC_EVENT_INFO_STATUS_RX1_ERROR;

        if( _lora_time.TimerGetElapsedTime( AggregatedLastTxDoneTime ) >= RxWindow2Delay )
        {
            _lora_time.TimerStop( &RxWindowTimer2 );
            LoRaMacFlags.Bits.MacDone = 1;
        }
    }
    else
    {
        if( NodeAckRequested == true )
        {
            McpsConfirm.Status = LORAMAC_EVENT_INFO_STATUS_RX2_ERROR;
        }
        MlmeConfirm.Status = LORAMAC_EVENT_INFO_STATUS_RX2_ERROR;
        LoRaMacFlags.Bits.MacDone = 1;
    }
}

void LoRaMac::OnRadioRxTimeout( void )
{
    if( LoRaMacDeviceClass != CLASS_C )
    {
        lora_phy->put_radio_to_sleep();
    }
    else
    {
        OpenContinuousRx2Window( );
    }

    if( RxSlot == RX_SLOT_WIN_1 )
    {
        if( NodeAckRequested == true )
        {
            McpsConfirm.Status = LORAMAC_EVENT_INFO_STATUS_RX1_TIMEOUT;
        }
        MlmeConfirm.Status = LORAMAC_EVENT_INFO_STATUS_RX1_TIMEOUT;

        if( _lora_time.TimerGetElapsedTime( AggregatedLastTxDoneTime ) >= RxWindow2Delay )
        {
            _lora_time.TimerStop( &RxWindowTimer2 );
            LoRaMacFlags.Bits.MacDone = 1;
        }
    }
    else
    {
        if( NodeAckRequested == true )
        {
            McpsConfirm.Status = LORAMAC_EVENT_INFO_STATUS_RX2_TIMEOUT;
        }
        MlmeConfirm.Status = LORAMAC_EVENT_INFO_STATUS_RX2_TIMEOUT;

        if( LoRaMacDeviceClass != CLASS_C )
        {
            LoRaMacFlags.Bits.MacDone = 1;
        }
    }
}

/***************************************************************************
 * Timer event callbacks - deliberated locally                             *
 **************************************************************************/
void LoRaMac::OnMacStateCheckTimerEvent( void )
{
    GetPhyParams_t getPhy;
    PhyParam_t phyParam;
    bool txTimeout = false;

    _lora_time.TimerStop( &MacStateCheckTimer );

    if( LoRaMacFlags.Bits.MacDone == 1 )
    {
        if( ( LoRaMacState & LORAMAC_RX_ABORT ) == LORAMAC_RX_ABORT )
        {
            LoRaMacState &= ~LORAMAC_RX_ABORT;
            LoRaMacState &= ~LORAMAC_TX_RUNNING;
        }

        if( ( LoRaMacFlags.Bits.MlmeReq == 1 ) || ( ( LoRaMacFlags.Bits.McpsReq == 1 ) ) )
        {
            if( ( McpsConfirm.Status == LORAMAC_EVENT_INFO_STATUS_TX_TIMEOUT ) ||
                ( MlmeConfirm.Status == LORAMAC_EVENT_INFO_STATUS_TX_TIMEOUT ) )
            {
                // Stop transmit cycle due to tx timeout.
                LoRaMacState &= ~LORAMAC_TX_RUNNING;
                mac_commands.ClearCommandBuffer();
                McpsConfirm.NbRetries = AckTimeoutRetriesCounter;
                McpsConfirm.AckReceived = false;
                McpsConfirm.TxTimeOnAir = 0;
                txTimeout = true;
            }
        }

        if( ( NodeAckRequested == false ) && ( txTimeout == false ) )
        {
            if( ( LoRaMacFlags.Bits.MlmeReq == 1 ) || ( ( LoRaMacFlags.Bits.McpsReq == 1 ) ) )
            {
                if( ( LoRaMacFlags.Bits.MlmeReq == 1 ) && ( MlmeConfirm.MlmeRequest == MLME_JOIN ) )
                {// Procedure for the join request
                    MlmeConfirm.NbRetries = JoinRequestTrials;

                    if( MlmeConfirm.Status == LORAMAC_EVENT_INFO_STATUS_OK )
                    {// Node joined successfully
                        UpLinkCounter = 0;
                        ChannelsNbRepCounter = 0;
                        LoRaMacState &= ~LORAMAC_TX_RUNNING;
                    }
                    else
                    {
                        if( JoinRequestTrials >= MaxJoinRequestTrials )
                        {
                            LoRaMacState &= ~LORAMAC_TX_RUNNING;
                        }
                        else
                        {
                            LoRaMacFlags.Bits.MacDone = 0;
                            // Sends the same frame again
                            handle_delayed_tx_timer_event();
                        }
                    }
                }
                else
                {// Procedure for all other frames
                    if( ( ChannelsNbRepCounter >= LoRaMacParams.ChannelsNbRep ) || ( LoRaMacFlags.Bits.McpsInd == 1 ) )
                    {
                        if( LoRaMacFlags.Bits.McpsInd == 0 )
                        {   // Maximum repetitions without downlink. Reset MacCommandsBufferIndex. Increase ADR Ack counter.
                            // Only process the case when the MAC did not receive a downlink.
                            mac_commands.ClearCommandBuffer();
                            AdrAckCounter++;
                        }

                        ChannelsNbRepCounter = 0;

                        if( IsUpLinkCounterFixed == false )
                        {
                            UpLinkCounter++;
                        }

                        LoRaMacState &= ~LORAMAC_TX_RUNNING;
                    }
                    else
                    {
                        LoRaMacFlags.Bits.MacDone = 0;
                        // Sends the same frame again
                        handle_delayed_tx_timer_event();
                    }
                }
            }
        }

        if( LoRaMacFlags.Bits.McpsInd == 1 )
        {// Procedure if we received a frame
            if( ( McpsConfirm.AckReceived == true ) || ( AckTimeoutRetriesCounter > AckTimeoutRetries ) )
            {
                AckTimeoutRetry = false;
                NodeAckRequested = false;
                if( IsUpLinkCounterFixed == false )
                {
                    UpLinkCounter++;
                }
                McpsConfirm.NbRetries = AckTimeoutRetriesCounter;

                LoRaMacState &= ~LORAMAC_TX_RUNNING;
            }
        }

        if( ( AckTimeoutRetry == true ) && ( ( LoRaMacState & LORAMAC_TX_DELAYED ) == 0 ) )
        {// Retransmissions procedure for confirmed uplinks
            AckTimeoutRetry = false;
            if( ( AckTimeoutRetriesCounter < AckTimeoutRetries ) && ( AckTimeoutRetriesCounter <= MAX_ACK_RETRIES ) )
            {
                AckTimeoutRetriesCounter++;

                if( ( AckTimeoutRetriesCounter % 2 ) == 1 )
                {
                    getPhy.Attribute = PHY_NEXT_LOWER_TX_DR;
                    getPhy.UplinkDwellTime = LoRaMacParams.UplinkDwellTime;
                    getPhy.Datarate = LoRaMacParams.ChannelsDatarate;
                    phyParam = lora_phy->get_phy_params( &getPhy );
                    LoRaMacParams.ChannelsDatarate = phyParam.Value;
                }
                // Try to send the frame again
                if( ScheduleTx( ) == LORAMAC_STATUS_OK )
                {
                    LoRaMacFlags.Bits.MacDone = 0;
                }
                else
                {
                    // The DR is not applicable for the payload size
                    McpsConfirm.Status = LORAMAC_EVENT_INFO_STATUS_TX_DR_PAYLOAD_SIZE_ERROR;

                    mac_commands.ClearCommandBuffer();
                    LoRaMacState &= ~LORAMAC_TX_RUNNING;
                    NodeAckRequested = false;
                    McpsConfirm.AckReceived = false;
                    McpsConfirm.NbRetries = AckTimeoutRetriesCounter;
                    McpsConfirm.Datarate = LoRaMacParams.ChannelsDatarate;
                    if( IsUpLinkCounterFixed == false )
                    {
                        UpLinkCounter++;
                    }
                }
            }
            else
            {
                lora_phy->load_defaults(INIT_TYPE_RESTORE);

                LoRaMacState &= ~LORAMAC_TX_RUNNING;

                mac_commands.ClearCommandBuffer();
                NodeAckRequested = false;
                McpsConfirm.AckReceived = false;
                McpsConfirm.NbRetries = AckTimeoutRetriesCounter;
                if( IsUpLinkCounterFixed == false )
                {
                    UpLinkCounter++;
                }
            }
        }
    }
    // Handle reception for Class B and Class C
    if( ( LoRaMacState & LORAMAC_RX ) == LORAMAC_RX )
    {
        LoRaMacState &= ~LORAMAC_RX;
    }
    if( LoRaMacState == LORAMAC_IDLE )
    {
        if( LoRaMacFlags.Bits.McpsReq == 1 )
        {
            LoRaMacFlags.Bits.McpsReq = 0;
            LoRaMacPrimitives->MacMcpsConfirm( &McpsConfirm );
        }

        if( LoRaMacFlags.Bits.MlmeReq == 1 )
        {
            LoRaMacFlags.Bits.MlmeReq = 0;
            LoRaMacPrimitives->MacMlmeConfirm( &MlmeConfirm );
        }

        // Verify if sticky MAC commands are pending or not
        if( mac_commands.IsStickyMacCommandPending( ) == true )
        {// Setup MLME indication
            SetMlmeScheduleUplinkIndication( );
        }

        // Procedure done. Reset variables.
        LoRaMacFlags.Bits.MacDone = 0;
    }
    else
    {
        // Operation not finished restart timer
        _lora_time.TimerSetValue( &MacStateCheckTimer, MAC_STATE_CHECK_TIMEOUT );
        _lora_time.TimerStart( &MacStateCheckTimer );
    }

    // Handle MCPS indication
    if( LoRaMacFlags.Bits.McpsInd == 1 )
    {
        LoRaMacFlags.Bits.McpsInd = 0;
        if( LoRaMacDeviceClass == CLASS_C )
        {// Activate RX2 window for Class C
            OpenContinuousRx2Window( );
        }
        if( LoRaMacFlags.Bits.McpsIndSkip == 0 )
        {
            LoRaMacPrimitives->MacMcpsIndication( &McpsIndication );
        }
        LoRaMacFlags.Bits.McpsIndSkip = 0;
    }

    // Handle MLME indication
    if( LoRaMacFlags.Bits.MlmeInd == 1 )
    {
        LoRaMacFlags.Bits.MlmeInd = 0;
        LoRaMacPrimitives->MacMlmeIndication( &MlmeIndication );
    }
}

void LoRaMac::OnTxDelayedTimerEvent( void )
{
    LoRaMacHeader_t macHdr;
    LoRaMacFrameCtrl_t fCtrl;
    AlternateDrParams_t altDr;

    _lora_time.TimerStop( &TxDelayedTimer );
    LoRaMacState &= ~LORAMAC_TX_DELAYED;

    if( ( LoRaMacFlags.Bits.MlmeReq == 1 ) && ( MlmeConfirm.MlmeRequest == MLME_JOIN ) )
    {
        ResetMacParameters( );

        altDr.NbTrials = JoinRequestTrials + 1;
        LoRaMacParams.ChannelsDatarate = lora_phy->get_alternate_DR(&altDr);

        macHdr.Value = 0;
        macHdr.Bits.MType = FRAME_TYPE_JOIN_REQ;

        fCtrl.Value = 0;
        fCtrl.Bits.Adr = LoRaMacParams.AdrCtrlOn;

        /* In case of join request retransmissions, the stack must prepare
         * the frame again, because the network server keeps track of the random
         * LoRaMacDevNonce values to prevent reply attacks. */
        PrepareFrame( &macHdr, &fCtrl, 0, NULL, 0 );
    }

    ScheduleTx( );
}

void LoRaMac::OnRxWindow1TimerEvent( void )
{
    _lora_time.TimerStop( &RxWindowTimer1 );
    RxSlot = RX_SLOT_WIN_1;

    RxWindow1Config.Channel = Channel;
    RxWindow1Config.DrOffset = LoRaMacParams.Rx1DrOffset;
    RxWindow1Config.DownlinkDwellTime = LoRaMacParams.DownlinkDwellTime;
    RxWindow1Config.RepeaterSupport = RepeaterSupport;
    RxWindow1Config.RxContinuous = false;
    RxWindow1Config.RxSlot = RxSlot;

    if( LoRaMacDeviceClass == CLASS_C )
    {
        lora_phy->put_radio_to_standby();
    }

    lora_phy->rx_config(&RxWindow1Config, ( int8_t* )&McpsIndication.RxDatarate);
    RxWindowSetup( RxWindow1Config.RxContinuous, LoRaMacParams.MaxRxWindow );
}

void LoRaMac::OnRxWindow2TimerEvent( void )
{
    _lora_time.TimerStop( &RxWindowTimer2 );

    RxWindow2Config.Channel = Channel;
    RxWindow2Config.Frequency = LoRaMacParams.Rx2Channel.Frequency;
    RxWindow2Config.DownlinkDwellTime = LoRaMacParams.DownlinkDwellTime;
    RxWindow2Config.RepeaterSupport = RepeaterSupport;
    RxWindow2Config.RxSlot = RX_SLOT_WIN_2;

    if( LoRaMacDeviceClass != CLASS_C )
    {
        RxWindow2Config.RxContinuous = false;
    }
    else
    {
        // Setup continuous listening for class c
        RxWindow2Config.RxContinuous = true;
    }

    if(lora_phy->rx_config(&RxWindow2Config, ( int8_t* )&McpsIndication.RxDatarate) == true )
    {
        RxWindowSetup( RxWindow2Config.RxContinuous, LoRaMacParams.MaxRxWindow );
        RxSlot = RX_SLOT_WIN_2;
    }
}

void LoRaMac::OnAckTimeoutTimerEvent( void )
{
    _lora_time.TimerStop( &AckTimeoutTimer );

    if( NodeAckRequested == true )
    {
        AckTimeoutRetry = true;
        LoRaMacState &= ~LORAMAC_ACK_REQ;
    }
    if( LoRaMacDeviceClass == CLASS_C )
    {
        LoRaMacFlags.Bits.MacDone = 1;
    }
}

void LoRaMac::RxWindowSetup( bool rxContinuous, uint32_t maxRxWindow )
{
    lora_phy->setup_rx_window(rxContinuous, maxRxWindow);
}

bool LoRaMac::ValidatePayloadLength( uint8_t lenN, int8_t datarate, uint8_t fOptsLen )
{
    GetPhyParams_t getPhy;
    PhyParam_t phyParam;
    uint16_t maxN = 0;
    uint16_t payloadSize = 0;

    // Setup PHY request
    getPhy.UplinkDwellTime = LoRaMacParams.UplinkDwellTime;
    getPhy.Datarate = datarate;
    getPhy.Attribute = PHY_MAX_PAYLOAD;

    // Get the maximum payload length
    if( RepeaterSupport == true )
    {
        getPhy.Attribute = PHY_MAX_PAYLOAD_REPEATER;
    }
    phyParam = lora_phy->get_phy_params(&getPhy);
    maxN = phyParam.Value;

    // Calculate the resulting payload size
    payloadSize = ( lenN + fOptsLen );

    // Validation of the application payload size
    if( ( payloadSize <= maxN ) && ( payloadSize <= LORAMAC_PHY_MAXPAYLOAD ) )
    {
        return true;
    }
    return false;
}

void LoRaMac::SetMlmeScheduleUplinkIndication( void )
{
    MlmeIndication.MlmeIndication = MLME_SCHEDULE_UPLINK;
    LoRaMacFlags.Bits.MlmeInd = 1;
}

// This is not actual transmission. It just schedules a message in response
// to MCPS request
LoRaMacStatus_t LoRaMac::Send( LoRaMacHeader_t *macHdr, uint8_t fPort, void *fBuffer, uint16_t fBufferSize )
{
    LoRaMacFrameCtrl_t fCtrl;
    LoRaMacStatus_t status = LORAMAC_STATUS_PARAMETER_INVALID;

    fCtrl.Value = 0;
    fCtrl.Bits.FOptsLen      = 0;
    fCtrl.Bits.FPending      = 0;
    fCtrl.Bits.Ack           = false;
    fCtrl.Bits.AdrAckReq     = false;
    fCtrl.Bits.Adr           = LoRaMacParams.AdrCtrlOn;

    // Prepare the frame
    status = PrepareFrame( macHdr, &fCtrl, fPort, fBuffer, fBufferSize );

    // Validate status
    if( status != LORAMAC_STATUS_OK )
    {
        return status;
    }

    // Reset confirm parameters
    McpsConfirm.NbRetries = 0;
    McpsConfirm.AckReceived = false;
    McpsConfirm.UpLinkCounter = UpLinkCounter;

    status = ScheduleTx( );

    return status;
}

LoRaMacStatus_t LoRaMac::ScheduleTx( void )
{
    TimerTime_t dutyCycleTimeOff = 0;
    NextChanParams_t nextChan;

    // Check if the device is off
    if( LoRaMacParams.MaxDCycle == 255 )
    {
        return LORAMAC_STATUS_DEVICE_OFF;
    }
    if( LoRaMacParams.MaxDCycle == 0 )
    {
        AggregatedTimeOff = 0;
    }

    // Update Backoff
    CalculateBackOff( LastTxChannel );

    nextChan.AggrTimeOff = AggregatedTimeOff;
    nextChan.Datarate = LoRaMacParams.ChannelsDatarate;
    DutyCycleOn = LORAWAN_DUTYCYCLE_ON;
    nextChan.DutyCycleEnabled = DutyCycleOn;
    nextChan.Joined = IsLoRaMacNetworkJoined;
    nextChan.LastAggrTx = AggregatedLastTxDoneTime;

    // Select channel
    while( lora_phy->set_next_channel(&nextChan, &Channel, &dutyCycleTimeOff, &AggregatedTimeOff ) == false )
    {
        // Set the default datarate
        LoRaMacParams.ChannelsDatarate = LoRaMacParamsDefaults.ChannelsDatarate;
        // Update datarate in the function parameters
        nextChan.Datarate = LoRaMacParams.ChannelsDatarate;
    }

    tr_debug("Next Channel Idx=%d, DR=%d", Channel, nextChan.Datarate);

    // Compute Rx1 windows parameters
    uint8_t dr_offset = lora_phy->apply_DR_offset(LoRaMacParams.DownlinkDwellTime,
                                                 LoRaMacParams.ChannelsDatarate,
                                                 LoRaMacParams.Rx1DrOffset);

    lora_phy->compute_rx_win_params(dr_offset, LoRaMacParams.MinRxSymbols,
                                     LoRaMacParams.SystemMaxRxError,
                                     &RxWindow1Config );
    // Compute Rx2 windows parameters
    lora_phy->compute_rx_win_params(LoRaMacParams.Rx2Channel.Datarate,
                                    LoRaMacParams.MinRxSymbols,
                                    LoRaMacParams.SystemMaxRxError,
                                    &RxWindow2Config );

    if( IsLoRaMacNetworkJoined == false )
    {
        RxWindow1Delay = LoRaMacParams.JoinAcceptDelay1 + RxWindow1Config.WindowOffset;
        RxWindow2Delay = LoRaMacParams.JoinAcceptDelay2 + RxWindow2Config.WindowOffset;
    }
    else
    {
        if( ValidatePayloadLength( LoRaMacTxPayloadLen, LoRaMacParams.ChannelsDatarate, mac_commands.GetLength() ) == false )
        {
            return LORAMAC_STATUS_LENGTH_ERROR;
        }
        RxWindow1Delay = LoRaMacParams.ReceiveDelay1 + RxWindow1Config.WindowOffset;
        RxWindow2Delay = LoRaMacParams.ReceiveDelay2 + RxWindow2Config.WindowOffset;
    }

    // Schedule transmission of frame
    if( dutyCycleTimeOff == 0 )
    {
        // Try to send now
        return SendFrameOnChannel( Channel );
    }
    else
    {
        // Send later - prepare timer
        LoRaMacState |= LORAMAC_TX_DELAYED;
        tr_debug("Next Transmission in %lu ms", dutyCycleTimeOff);

        _lora_time.TimerSetValue( &TxDelayedTimer, dutyCycleTimeOff );
        _lora_time.TimerStart( &TxDelayedTimer );

        return LORAMAC_STATUS_OK;
    }
}

void LoRaMac::CalculateBackOff( uint8_t channel )
{
    CalcBackOffParams_t calcBackOff;

    calcBackOff.Joined = IsLoRaMacNetworkJoined;
    DutyCycleOn = LORAWAN_DUTYCYCLE_ON;
    calcBackOff.DutyCycleEnabled = DutyCycleOn;
    calcBackOff.Channel = channel;
    calcBackOff.ElapsedTime = _lora_time.TimerGetElapsedTime( LoRaMacInitializationTime );
    calcBackOff.TxTimeOnAir = TxTimeOnAir;
    calcBackOff.LastTxIsJoinRequest = LastTxIsJoinRequest;

    // Update regional back-off
    lora_phy->calculate_backoff(&calcBackOff);

    // Update aggregated time-off
    AggregatedTimeOff = AggregatedTimeOff + ( TxTimeOnAir * LoRaMacParams.AggregatedDCycle - TxTimeOnAir );
}

void LoRaMac::ResetMacParameters( void )
{
    IsLoRaMacNetworkJoined = false;

    // Counters
    UpLinkCounter = 0;
    DownLinkCounter = 0;
    AdrAckCounter = 0;

    ChannelsNbRepCounter = 0;

    AckTimeoutRetries = 1;
    AckTimeoutRetriesCounter = 1;
    AckTimeoutRetry = false;

    LoRaMacParams.MaxDCycle = 0;
    LoRaMacParams.AggregatedDCycle = 1;

    mac_commands.ClearCommandBuffer();
    mac_commands.ClearRepeatBuffer();
    mac_commands.ClearMacCommandsInNextTx();

    IsRxWindowsEnabled = true;

    LoRaMacParams.ChannelsTxPower = LoRaMacParamsDefaults.ChannelsTxPower;
    LoRaMacParams.ChannelsDatarate = LoRaMacParamsDefaults.ChannelsDatarate;
    LoRaMacParams.Rx1DrOffset = LoRaMacParamsDefaults.Rx1DrOffset;
    LoRaMacParams.Rx2Channel = LoRaMacParamsDefaults.Rx2Channel;
    LoRaMacParams.UplinkDwellTime = LoRaMacParamsDefaults.UplinkDwellTime;
    LoRaMacParams.DownlinkDwellTime = LoRaMacParamsDefaults.DownlinkDwellTime;
    LoRaMacParams.MaxEirp = LoRaMacParamsDefaults.MaxEirp;
    LoRaMacParams.AntennaGain = LoRaMacParamsDefaults.AntennaGain;

    NodeAckRequested = false;
    SrvAckRequested = false;

    // Reset Multicast downlink counters
    MulticastParams_t *cur = MulticastChannels;
    while( cur != NULL )
    {
        cur->DownLinkCounter = 0;
        cur = cur->Next;
    }

    // Initialize channel index.
    Channel = 0;
    LastTxChannel = Channel;
}

bool LoRaMac::IsFPortAllowed( uint8_t fPort )
{
    if( ( fPort == 0 ) || ( fPort > 224 ) )
    {
        return false;
    }
    return true;
}

void LoRaMac::OpenContinuousRx2Window( void )
{
    handle_rx2_timer_event( );
    RxSlot = RX_SLOT_WIN_CLASS_C;
}

static void memcpy_convert_endianess( uint8_t *dst, const uint8_t *src, uint16_t size )
{
    dst = dst + ( size - 1 );
    while( size-- )
    {
        *dst-- = *src++;
    }
}

LoRaMacStatus_t LoRaMac::PrepareFrame( LoRaMacHeader_t *macHdr, LoRaMacFrameCtrl_t *fCtrl, uint8_t fPort, void *fBuffer, uint16_t fBufferSize )
{
    AdrNextParams_t adrNext;
    uint16_t i;
    uint8_t pktHeaderLen = 0;
    uint32_t mic = 0;
    const void* payload = fBuffer;
    uint8_t framePort = fPort;
    LoRaMacStatus_t status = LORAMAC_STATUS_OK;

    LoRaMacBufferPktLen = 0;

    NodeAckRequested = false;

    if( fBuffer == NULL )
    {
        fBufferSize = 0;
    }

    LoRaMacTxPayloadLen = fBufferSize;

    LoRaMacBuffer[pktHeaderLen++] = macHdr->Value;

    switch( macHdr->Bits.MType )
    {
        case FRAME_TYPE_JOIN_REQ:
            LoRaMacBufferPktLen = pktHeaderLen;

            memcpy_convert_endianess( LoRaMacBuffer + LoRaMacBufferPktLen, LoRaMacAppEui, 8 );
            LoRaMacBufferPktLen += 8;
            memcpy_convert_endianess( LoRaMacBuffer + LoRaMacBufferPktLen, LoRaMacDevEui, 8 );
            LoRaMacBufferPktLen += 8;

            LoRaMacDevNonce = lora_phy->get_radio_rng();

            LoRaMacBuffer[LoRaMacBufferPktLen++] = LoRaMacDevNonce & 0xFF;
            LoRaMacBuffer[LoRaMacBufferPktLen++] = ( LoRaMacDevNonce >> 8 ) & 0xFF;

            if (0 != LoRaMacJoinComputeMic( LoRaMacBuffer, LoRaMacBufferPktLen & 0xFF, LoRaMacAppKey, &mic )) {
                return LORAMAC_STATUS_CRYPTO_FAIL;
            }

            LoRaMacBuffer[LoRaMacBufferPktLen++] = mic & 0xFF;
            LoRaMacBuffer[LoRaMacBufferPktLen++] = ( mic >> 8 ) & 0xFF;
            LoRaMacBuffer[LoRaMacBufferPktLen++] = ( mic >> 16 ) & 0xFF;
            LoRaMacBuffer[LoRaMacBufferPktLen++] = ( mic >> 24 ) & 0xFF;

            break;
        case FRAME_TYPE_DATA_CONFIRMED_UP:
            NodeAckRequested = true;
            //Intentional fallthrough
        case FRAME_TYPE_DATA_UNCONFIRMED_UP:
        {
            if( IsLoRaMacNetworkJoined == false )
            {
                return LORAMAC_STATUS_NO_NETWORK_JOINED; // No network has been joined yet
            }

            // Adr next request
            adrNext.UpdateChanMask = true;
            adrNext.AdrEnabled = fCtrl->Bits.Adr;
            adrNext.AdrAckCounter = AdrAckCounter;
            adrNext.Datarate = LoRaMacParams.ChannelsDatarate;
            adrNext.TxPower = LoRaMacParams.ChannelsTxPower;
            adrNext.UplinkDwellTime = LoRaMacParams.UplinkDwellTime;

            fCtrl->Bits.AdrAckReq = lora_phy->get_next_ADR(&adrNext,
                                                          &LoRaMacParams.ChannelsDatarate,
                                                          &LoRaMacParams.ChannelsTxPower,
                                                          &AdrAckCounter);

            if( SrvAckRequested == true )
            {
                SrvAckRequested = false;
                fCtrl->Bits.Ack = 1;
            }

            LoRaMacBuffer[pktHeaderLen++] = ( LoRaMacDevAddr ) & 0xFF;
            LoRaMacBuffer[pktHeaderLen++] = ( LoRaMacDevAddr >> 8 ) & 0xFF;
            LoRaMacBuffer[pktHeaderLen++] = ( LoRaMacDevAddr >> 16 ) & 0xFF;
            LoRaMacBuffer[pktHeaderLen++] = ( LoRaMacDevAddr >> 24 ) & 0xFF;

            LoRaMacBuffer[pktHeaderLen++] = fCtrl->Value;

            LoRaMacBuffer[pktHeaderLen++] = UpLinkCounter & 0xFF;
            LoRaMacBuffer[pktHeaderLen++] = ( UpLinkCounter >> 8 ) & 0xFF;

            // Copy the MAC commands which must be re-send into the MAC command buffer
            mac_commands.CopyRepeatCommandsToBuffer();

            const uint8_t mac_commands_len = mac_commands.GetLength();
            if( ( payload != NULL ) && ( LoRaMacTxPayloadLen > 0 ) )
            {
                if( mac_commands.IsMacCommandsInNextTx() == true )
                {
                    if( mac_commands_len <= LORA_MAC_COMMAND_MAX_FOPTS_LENGTH )
                    {
                        fCtrl->Bits.FOptsLen += mac_commands_len;

                        // Update FCtrl field with new value of OptionsLength
                        LoRaMacBuffer[0x05] = fCtrl->Value;

                        const uint8_t *buffer = mac_commands.GetMacCommandsBuffer();
                        for( i = 0; i < mac_commands_len; i++ )
                        {
                            LoRaMacBuffer[pktHeaderLen++] = buffer[i];
                        }
                    }
                    else
                    {
                        LoRaMacTxPayloadLen = mac_commands_len;
                        payload = mac_commands.GetMacCommandsBuffer();
                        framePort = 0;
                    }
                }
            }
            else
            {
                if( ( mac_commands_len > 0 ) && ( mac_commands.IsMacCommandsInNextTx() == true ) )
                {
                    LoRaMacTxPayloadLen = mac_commands_len;
                    payload = mac_commands.GetMacCommandsBuffer();
                    framePort = 0;
                }
            }

            // Store MAC commands which must be re-send in case the device does not receive a downlink anymore
            mac_commands.ParseMacCommandsToRepeat();

            if( ( payload != NULL ) && ( LoRaMacTxPayloadLen > 0 ) )
            {
                LoRaMacBuffer[pktHeaderLen++] = framePort;

                if( framePort == 0 )
                {
                    // Reset buffer index as the mac commands are being sent on port 0
                    mac_commands.ClearCommandBuffer();
                    if (0 != LoRaMacPayloadEncrypt( (uint8_t* ) payload, LoRaMacTxPayloadLen, LoRaMacNwkSKey, LoRaMacDevAddr, UP_LINK, UpLinkCounter, &LoRaMacBuffer[pktHeaderLen] )) {
                        status = LORAMAC_STATUS_CRYPTO_FAIL;
                    }
                }
                else
                {
                    if (0 != LoRaMacPayloadEncrypt( (uint8_t* ) payload, LoRaMacTxPayloadLen, LoRaMacAppSKey, LoRaMacDevAddr, UP_LINK, UpLinkCounter, &LoRaMacBuffer[pktHeaderLen] )) {
                        status = LORAMAC_STATUS_CRYPTO_FAIL;
                    }
                }
            }
            LoRaMacBufferPktLen = pktHeaderLen + LoRaMacTxPayloadLen;

            if (0 != LoRaMacComputeMic( LoRaMacBuffer, LoRaMacBufferPktLen, LoRaMacNwkSKey, LoRaMacDevAddr, UP_LINK, UpLinkCounter, &mic )) {
                status = LORAMAC_STATUS_CRYPTO_FAIL;
            }

            LoRaMacBuffer[LoRaMacBufferPktLen + 0] = mic & 0xFF;
            LoRaMacBuffer[LoRaMacBufferPktLen + 1] = ( mic >> 8 ) & 0xFF;
            LoRaMacBuffer[LoRaMacBufferPktLen + 2] = ( mic >> 16 ) & 0xFF;
            LoRaMacBuffer[LoRaMacBufferPktLen + 3] = ( mic >> 24 ) & 0xFF;

            LoRaMacBufferPktLen += LORAMAC_MFR_LEN;
        }
        break;
        case FRAME_TYPE_PROPRIETARY:
            if( ( fBuffer != NULL ) && ( LoRaMacTxPayloadLen > 0 ) )
            {
                memcpy( LoRaMacBuffer + pktHeaderLen, ( uint8_t* ) fBuffer, LoRaMacTxPayloadLen );
                LoRaMacBufferPktLen = pktHeaderLen + LoRaMacTxPayloadLen;
            }
            break;
        default:
            status = LORAMAC_STATUS_SERVICE_UNKNOWN;
    }

    return status;
}

LoRaMacStatus_t LoRaMac::SendFrameOnChannel( uint8_t channel )
{
    TxConfigParams_t txConfig;
    int8_t txPower = 0;

    txConfig.Channel = channel;
    txConfig.Datarate = LoRaMacParams.ChannelsDatarate;
    txConfig.TxPower = LoRaMacParams.ChannelsTxPower;
    txConfig.MaxEirp = LoRaMacParams.MaxEirp;
    txConfig.AntennaGain = LoRaMacParams.AntennaGain;
    txConfig.PktLen = LoRaMacBufferPktLen;

    lora_phy->tx_config(&txConfig, &txPower, &TxTimeOnAir);

    MlmeConfirm.Status = LORAMAC_EVENT_INFO_STATUS_ERROR;
    McpsConfirm.Status = LORAMAC_EVENT_INFO_STATUS_ERROR;
    McpsConfirm.Datarate = LoRaMacParams.ChannelsDatarate;
    McpsConfirm.TxPower = txPower;

    // Store the time on air
    McpsConfirm.TxTimeOnAir = TxTimeOnAir;
    MlmeConfirm.TxTimeOnAir = TxTimeOnAir;

    // Starts the MAC layer status check timer
    _lora_time.TimerSetValue( &MacStateCheckTimer, MAC_STATE_CHECK_TIMEOUT );
    _lora_time.TimerStart( &MacStateCheckTimer );

    if( IsLoRaMacNetworkJoined == false )
    {
        JoinRequestTrials++;
    }

    // Send now
    lora_phy->handle_send(LoRaMacBuffer, LoRaMacBufferPktLen);

    LoRaMacState |= LORAMAC_TX_RUNNING;

    return LORAMAC_STATUS_OK;
}

LoRaMacStatus_t LoRaMac::SetTxContinuousWave( uint16_t timeout )
{
    ContinuousWaveParams_t continuousWave;

    continuousWave.Channel = Channel;
    continuousWave.Datarate = LoRaMacParams.ChannelsDatarate;
    continuousWave.TxPower = LoRaMacParams.ChannelsTxPower;
    continuousWave.MaxEirp = LoRaMacParams.MaxEirp;
    continuousWave.AntennaGain = LoRaMacParams.AntennaGain;
    continuousWave.Timeout = timeout;

    lora_phy->set_tx_cont_mode(&continuousWave);

    // Starts the MAC layer status check timer
    _lora_time.TimerSetValue( &MacStateCheckTimer, MAC_STATE_CHECK_TIMEOUT );
    _lora_time.TimerStart( &MacStateCheckTimer );

    LoRaMacState |= LORAMAC_TX_RUNNING;

    return LORAMAC_STATUS_OK;
}

LoRaMacStatus_t LoRaMac::SetTxContinuousWave1( uint16_t timeout, uint32_t frequency, uint8_t power )
{
    lora_phy->setup_tx_cont_wave_mode(frequency, power, timeout);

    // Starts the MAC layer status check timer
    _lora_time.TimerSetValue( &MacStateCheckTimer, MAC_STATE_CHECK_TIMEOUT );
    _lora_time.TimerStart( &MacStateCheckTimer );

    LoRaMacState |= LORAMAC_TX_RUNNING;

    return LORAMAC_STATUS_OK;
}

LoRaMacStatus_t LoRaMac::LoRaMacInitialization(LoRaMacPrimitives_t *primitives,
                                               LoRaMacCallback_t *callbacks,
                                               LoRaPHY *phy,
                                               EventQueue *queue)
{
    GetPhyParams_t getPhy;
    PhyParam_t phyParam;

    //store event queue pointer
    ev_queue = queue;

    if(!primitives || !callbacks)
    {
        return LORAMAC_STATUS_PARAMETER_INVALID;
    }

    lora_phy = phy;

    LoRaMacPrimitives = primitives;
    LoRaMacCallbacks = callbacks;

    LoRaMacFlags.Value = 0;

    LoRaMacDeviceClass = CLASS_A;
    LoRaMacState = LORAMAC_IDLE;

    JoinRequestTrials = 0;
    MaxJoinRequestTrials = 1;
    RepeaterSupport = false;

    // Reset duty cycle times
    AggregatedLastTxDoneTime = 0;
    AggregatedTimeOff = 0;

    // Reset to defaults
    getPhy.Attribute = PHY_DUTY_CYCLE;
    phyParam = lora_phy->get_phy_params(&getPhy);
    // load default at this moment. Later can be changed using json
    DutyCycleOn = ( bool ) phyParam.Value;

    getPhy.Attribute = PHY_DEF_TX_POWER;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.ChannelsTxPower = phyParam.Value;

    getPhy.Attribute = PHY_DEF_TX_DR;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.ChannelsDatarate = phyParam.Value;

    getPhy.Attribute = PHY_MAX_RX_WINDOW;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.MaxRxWindow = phyParam.Value;

    getPhy.Attribute = PHY_RECEIVE_DELAY1;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.ReceiveDelay1 = phyParam.Value;

    getPhy.Attribute = PHY_RECEIVE_DELAY2;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.ReceiveDelay2 = phyParam.Value;

    getPhy.Attribute = PHY_JOIN_ACCEPT_DELAY1;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.JoinAcceptDelay1 = phyParam.Value;

    getPhy.Attribute = PHY_JOIN_ACCEPT_DELAY2;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.JoinAcceptDelay2 = phyParam.Value;

    getPhy.Attribute = PHY_DEF_DR1_OFFSET;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.Rx1DrOffset = phyParam.Value;

    getPhy.Attribute = PHY_DEF_RX2_FREQUENCY;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.Rx2Channel.Frequency = phyParam.Value;

    getPhy.Attribute = PHY_DEF_RX2_DR;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.Rx2Channel.Datarate = phyParam.Value;

    getPhy.Attribute = PHY_DEF_UPLINK_DWELL_TIME;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.UplinkDwellTime = phyParam.Value;

    getPhy.Attribute = PHY_DEF_DOWNLINK_DWELL_TIME;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.DownlinkDwellTime = phyParam.Value;

    getPhy.Attribute = PHY_DEF_MAX_EIRP;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.MaxEirp = phyParam.fValue;

    getPhy.Attribute = PHY_DEF_ANTENNA_GAIN;
    phyParam = lora_phy->get_phy_params( &getPhy );
    LoRaMacParamsDefaults.AntennaGain = phyParam.fValue;

    lora_phy->load_defaults(INIT_TYPE_INIT);

    // Init parameters which are not set in function ResetMacParameters
    LoRaMacParamsDefaults.ChannelsNbRep = 1;
    LoRaMacParamsDefaults.SystemMaxRxError = 10;
    LoRaMacParamsDefaults.MinRxSymbols = 6;

    LoRaMacParams.SystemMaxRxError = LoRaMacParamsDefaults.SystemMaxRxError;
    LoRaMacParams.MinRxSymbols = LoRaMacParamsDefaults.MinRxSymbols;
    LoRaMacParams.MaxRxWindow = LoRaMacParamsDefaults.MaxRxWindow;
    LoRaMacParams.ReceiveDelay1 = LoRaMacParamsDefaults.ReceiveDelay1;
    LoRaMacParams.ReceiveDelay2 = LoRaMacParamsDefaults.ReceiveDelay2;
    LoRaMacParams.JoinAcceptDelay1 = LoRaMacParamsDefaults.JoinAcceptDelay1;
    LoRaMacParams.JoinAcceptDelay2 = LoRaMacParamsDefaults.JoinAcceptDelay2;
    LoRaMacParams.ChannelsNbRep = LoRaMacParamsDefaults.ChannelsNbRep;

    ResetMacParameters( );

    // Random seed initialization
    srand(lora_phy->get_radio_rng());

    PublicNetwork = LORAWAN_PUBLIC_NETWORK;
    lora_phy->setup_public_network_mode(PublicNetwork);
    lora_phy->put_radio_to_sleep();

    // Initialize timers
    _lora_time.TimerInit(&MacStateCheckTimer, mbed::callback(this, &LoRaMac::handle_mac_state_check_timer_event));
    _lora_time.TimerSetValue(&MacStateCheckTimer, MAC_STATE_CHECK_TIMEOUT);

    _lora_time.TimerInit(&TxDelayedTimer, mbed::callback(this, &LoRaMac::handle_delayed_tx_timer_event));
    _lora_time.TimerInit(&RxWindowTimer1, mbed::callback(this, &LoRaMac::handle_rx1_timer_event));
    _lora_time.TimerInit(&RxWindowTimer2, mbed::callback(this, &LoRaMac::handle_rx2_timer_event));
    _lora_time.TimerInit(&AckTimeoutTimer, mbed::callback(this, &LoRaMac::handle_ack_timeout));

    // Store the current initialization time
    LoRaMacInitializationTime = _lora_time.TimerGetCurrentTime();

    return LORAMAC_STATUS_OK;
}

LoRaMacStatus_t LoRaMac::LoRaMacQueryTxPossible( uint8_t size, LoRaMacTxInfo_t* txInfo )
{
    AdrNextParams_t adrNext;
    GetPhyParams_t getPhy;
    PhyParam_t phyParam;
    int8_t datarate = LoRaMacParamsDefaults.ChannelsDatarate;
    int8_t txPower = LoRaMacParamsDefaults.ChannelsTxPower;
    uint8_t fOptLen = mac_commands.GetLength() + mac_commands.GetRepeatLength();

    if( txInfo == NULL )
    {
        return LORAMAC_STATUS_PARAMETER_INVALID;
    }

    // Setup ADR request
    adrNext.UpdateChanMask = false;
    adrNext.AdrEnabled = LoRaMacParams.AdrCtrlOn;
    adrNext.AdrAckCounter = AdrAckCounter;
    adrNext.Datarate = LoRaMacParams.ChannelsDatarate;
    adrNext.TxPower = LoRaMacParams.ChannelsTxPower;
    adrNext.UplinkDwellTime = LoRaMacParams.UplinkDwellTime;

    // We call the function for information purposes only. We don't want to
    // apply the datarate, the tx power and the ADR ack counter.
    lora_phy->get_next_ADR(&adrNext, &datarate, &txPower, &AdrAckCounter);

    // Setup PHY request
    getPhy.UplinkDwellTime = LoRaMacParams.UplinkDwellTime;
    getPhy.Datarate = datarate;
    getPhy.Attribute = PHY_MAX_PAYLOAD;

    // Change request in case repeater is supported
    if( RepeaterSupport == true )
    {
        getPhy.Attribute = PHY_MAX_PAYLOAD_REPEATER;
    }
    phyParam = lora_phy->get_phy_params( &getPhy );
    txInfo->CurrentPayloadSize = phyParam.Value;

    // Verify if the fOpts fit into the maximum payload
    if( txInfo->CurrentPayloadSize >= fOptLen )
    {
        txInfo->MaxPossiblePayload = txInfo->CurrentPayloadSize - fOptLen;
    }
    else
    {
        txInfo->MaxPossiblePayload = txInfo->CurrentPayloadSize;
        // The fOpts don't fit into the maximum payload. Omit the MAC commands to
        // ensure that another uplink is possible.
        fOptLen = 0;
        mac_commands.ClearCommandBuffer();
        mac_commands.ClearRepeatBuffer();
    }

    // Verify if the fOpts and the payload fit into the maximum payload
    if( ValidatePayloadLength( size, datarate, fOptLen ) == false )
    {
        return LORAMAC_STATUS_LENGTH_ERROR;
    }
    return LORAMAC_STATUS_OK;
}

LoRaMacStatus_t LoRaMac::LoRaMacMibGetRequestConfirm( MibRequestConfirm_t *mibGet )
{
    LoRaMacStatus_t status = LORAMAC_STATUS_OK;
    GetPhyParams_t getPhy;
    PhyParam_t phyParam;

    if( mibGet == NULL )
    {
        return LORAMAC_STATUS_PARAMETER_INVALID;
    }

    switch( mibGet->Type )
    {
        case MIB_DEVICE_CLASS:
        {
            mibGet->Param.Class = LoRaMacDeviceClass;
            break;
        }
        case MIB_NETWORK_JOINED:
        {
            mibGet->Param.IsNetworkJoined = IsLoRaMacNetworkJoined;
            break;
        }
        case MIB_ADR:
        {
            mibGet->Param.AdrEnable = LoRaMacParams.AdrCtrlOn;
            break;
        }
        case MIB_NET_ID:
        {
            mibGet->Param.NetID = LoRaMacNetID;
            break;
        }
        case MIB_DEV_ADDR:
        {
            mibGet->Param.DevAddr = LoRaMacDevAddr;
            break;
        }
        case MIB_NWK_SKEY:
        {
            mibGet->Param.NwkSKey = LoRaMacNwkSKey;
            break;
        }
        case MIB_APP_SKEY:
        {
            mibGet->Param.AppSKey = LoRaMacAppSKey;
            break;
        }
        case MIB_PUBLIC_NETWORK:
        {
            mibGet->Param.EnablePublicNetwork = PublicNetwork;
            break;
        }
        case MIB_REPEATER_SUPPORT:
        {
            mibGet->Param.EnableRepeaterSupport = RepeaterSupport;
            break;
        }
        case MIB_CHANNELS:
        {
            getPhy.Attribute = PHY_CHANNELS;
            phyParam = lora_phy->get_phy_params( &getPhy );

            mibGet->Param.ChannelList = phyParam.Channels;
            break;
        }
        case MIB_RX2_CHANNEL:
        {
            mibGet->Param.Rx2Channel = LoRaMacParams.Rx2Channel;
            break;
        }
        case MIB_RX2_DEFAULT_CHANNEL:
        {
            mibGet->Param.Rx2Channel = LoRaMacParamsDefaults.Rx2Channel;
            break;
        }
        case MIB_CHANNELS_DEFAULT_MASK:
        {
            getPhy.Attribute = PHY_CHANNELS_DEFAULT_MASK;
            phyParam = lora_phy->get_phy_params( &getPhy );

            mibGet->Param.ChannelsDefaultMask = phyParam.ChannelsMask;
            break;
        }
        case MIB_CHANNELS_MASK:
        {
            getPhy.Attribute = PHY_CHANNELS_MASK;
            phyParam = lora_phy->get_phy_params( &getPhy );

            mibGet->Param.ChannelsMask = phyParam.ChannelsMask;
            break;
        }
        case MIB_CHANNELS_NB_REP:
        {
            mibGet->Param.ChannelNbRep = LoRaMacParams.ChannelsNbRep;
            break;
        }
        case MIB_MAX_RX_WINDOW_DURATION:
        {
            mibGet->Param.MaxRxWindow = LoRaMacParams.MaxRxWindow;
            break;
        }
        case MIB_RECEIVE_DELAY_1:
        {
            mibGet->Param.ReceiveDelay1 = LoRaMacParams.ReceiveDelay1;
            break;
        }
        case MIB_RECEIVE_DELAY_2:
        {
            mibGet->Param.ReceiveDelay2 = LoRaMacParams.ReceiveDelay2;
            break;
        }
        case MIB_JOIN_ACCEPT_DELAY_1:
        {
            mibGet->Param.JoinAcceptDelay1 = LoRaMacParams.JoinAcceptDelay1;
            break;
        }
        case MIB_JOIN_ACCEPT_DELAY_2:
        {
            mibGet->Param.JoinAcceptDelay2 = LoRaMacParams.JoinAcceptDelay2;
            break;
        }
        case MIB_CHANNELS_DEFAULT_DATARATE:
        {
            mibGet->Param.ChannelsDefaultDatarate = LoRaMacParamsDefaults.ChannelsDatarate;
            break;
        }
        case MIB_CHANNELS_DATARATE:
        {
            mibGet->Param.ChannelsDatarate = LoRaMacParams.ChannelsDatarate;
            break;
        }
        case MIB_CHANNELS_DEFAULT_TX_POWER:
        {
            mibGet->Param.ChannelsDefaultTxPower = LoRaMacParamsDefaults.ChannelsTxPower;
            break;
        }
        case MIB_CHANNELS_TX_POWER:
        {
            mibGet->Param.ChannelsTxPower = LoRaMacParams.ChannelsTxPower;
            break;
        }
        case MIB_UPLINK_COUNTER:
        {
            mibGet->Param.UpLinkCounter = UpLinkCounter;
            break;
        }
        case MIB_DOWNLINK_COUNTER:
        {
            mibGet->Param.DownLinkCounter = DownLinkCounter;
            break;
        }
        case MIB_MULTICAST_CHANNEL:
        {
            mibGet->Param.MulticastList = MulticastChannels;
            break;
        }
        case MIB_SYSTEM_MAX_RX_ERROR:
        {
            mibGet->Param.SystemMaxRxError = LoRaMacParams.SystemMaxRxError;
            break;
        }
        case MIB_MIN_RX_SYMBOLS:
        {
            mibGet->Param.MinRxSymbols = LoRaMacParams.MinRxSymbols;
            break;
        }
        case MIB_ANTENNA_GAIN:
        {
            mibGet->Param.AntennaGain = LoRaMacParams.AntennaGain;
            break;
        }
        default:
            status = LORAMAC_STATUS_SERVICE_UNKNOWN;
            break;
    }

    return status;
}

LoRaMacStatus_t LoRaMac::LoRaMacMibSetRequestConfirm( MibRequestConfirm_t *mibSet )
{
    LoRaMacStatus_t status = LORAMAC_STATUS_OK;
    ChanMaskSetParams_t chanMaskSet;
    VerifyParams_t verify;

    if( mibSet == NULL )
    {
        return LORAMAC_STATUS_PARAMETER_INVALID;
    }

    switch( mibSet->Type )
    {
        case MIB_DEVICE_CLASS:
        {
            LoRaMacDeviceClass = mibSet->Param.Class;
            switch( LoRaMacDeviceClass )
            {
                case CLASS_A:
                {
                    // Set the radio into sleep to setup a defined state
                    lora_phy->put_radio_to_sleep();
                    break;
                }
                case CLASS_B:
                {
                    break;
                }
                case CLASS_C:
                {
                    // Set the NodeAckRequested indicator to default
                    NodeAckRequested = false;
                    // Set the radio into sleep mode in case we are still in RX mode
                    lora_phy->put_radio_to_sleep();
                    // Compute Rx2 windows parameters in case the RX2 datarate has changed
                    lora_phy->compute_rx_win_params( LoRaMacParams.Rx2Channel.Datarate,
                                                     LoRaMacParams.MinRxSymbols,
                                                     LoRaMacParams.SystemMaxRxError,
                                                     &RxWindow2Config );
                    OpenContinuousRx2Window( );
                    break;
                }
            }
            break;
        }
        case MIB_NETWORK_JOINED:
        {
            IsLoRaMacNetworkJoined = mibSet->Param.IsNetworkJoined;
            break;
        }
        case MIB_ADR:
        {
            LoRaMacParams.AdrCtrlOn = mibSet->Param.AdrEnable;
            break;
        }
        case MIB_NET_ID:
        {
            LoRaMacNetID = mibSet->Param.NetID;
            break;
        }
        case MIB_DEV_ADDR:
        {
            LoRaMacDevAddr = mibSet->Param.DevAddr;
            break;
        }
        case MIB_NWK_SKEY:
        {
            if( mibSet->Param.NwkSKey != NULL )
            {
                memcpy( LoRaMacNwkSKey, mibSet->Param.NwkSKey,
                               sizeof( LoRaMacNwkSKey ) );
            }
            else
            {
                status = LORAMAC_STATUS_PARAMETER_INVALID;
            }
            break;
        }
        case MIB_APP_SKEY:
        {
            if( mibSet->Param.AppSKey != NULL )
            {
                memcpy( LoRaMacAppSKey, mibSet->Param.AppSKey,
                               sizeof( LoRaMacAppSKey ) );
            }
            else
            {
                status = LORAMAC_STATUS_PARAMETER_INVALID;
            }
            break;
        }
        case MIB_PUBLIC_NETWORK:
        {
            PublicNetwork = mibSet->Param.EnablePublicNetwork;
            lora_phy->setup_public_network_mode(PublicNetwork);
            break;
        }
        case MIB_REPEATER_SUPPORT:
        {
             RepeaterSupport = mibSet->Param.EnableRepeaterSupport;
            break;
        }
        case MIB_RX2_CHANNEL:
        {
            verify.DatarateParams.Datarate = mibSet->Param.Rx2Channel.Datarate;
            verify.DatarateParams.DownlinkDwellTime = LoRaMacParams.DownlinkDwellTime;

            if( lora_phy->verify(&verify, PHY_RX_DR) == true )
            {
                LoRaMacParams.Rx2Channel = mibSet->Param.Rx2Channel;

                if( ( LoRaMacDeviceClass == CLASS_C ) && ( IsLoRaMacNetworkJoined == true ) )
                {
                    // We can only compute the RX window parameters directly, if we are already
                    // in class c mode and joined. We cannot setup an RX window in case of any other
                    // class type.
                    // Set the radio into sleep mode in case we are still in RX mode
                    lora_phy->put_radio_to_sleep();
                    // Compute Rx2 windows parameters
                    lora_phy->compute_rx_win_params(LoRaMacParams.Rx2Channel.Datarate,
                                                   LoRaMacParams.MinRxSymbols,
                                                   LoRaMacParams.SystemMaxRxError,
                                                   &RxWindow2Config);

                    OpenContinuousRx2Window( );
                }
            }
            else
            {
                status = LORAMAC_STATUS_PARAMETER_INVALID;
            }
            break;
        }
        case MIB_RX2_DEFAULT_CHANNEL:
        {
            verify.DatarateParams.Datarate = mibSet->Param.Rx2Channel.Datarate;
            verify.DatarateParams.DownlinkDwellTime = LoRaMacParams.DownlinkDwellTime;

            if( lora_phy->verify(&verify, PHY_RX_DR) == true )
            {
                LoRaMacParamsDefaults.Rx2Channel = mibSet->Param.Rx2DefaultChannel;
            }
            else
            {
                status = LORAMAC_STATUS_PARAMETER_INVALID;
            }
            break;
        }
        case MIB_CHANNELS_DEFAULT_MASK:
        {
            chanMaskSet.ChannelsMaskIn = mibSet->Param.ChannelsMask;
            chanMaskSet.ChannelsMaskType = CHANNELS_DEFAULT_MASK;

            if(lora_phy->set_channel_mask(&chanMaskSet) == false )
            {
                status = LORAMAC_STATUS_PARAMETER_INVALID;
            }
            break;
        }
        case MIB_CHANNELS_MASK:
        {
            chanMaskSet.ChannelsMaskIn = mibSet->Param.ChannelsMask;
            chanMaskSet.ChannelsMaskType = CHANNELS_MASK;

            if(lora_phy->set_channel_mask(&chanMaskSet) == false )
            {
                status = LORAMAC_STATUS_PARAMETER_INVALID;
            }
            break;
        }
        case MIB_CHANNELS_NB_REP:
        {
            if( ( mibSet->Param.ChannelNbRep >= 1 ) &&
                ( mibSet->Param.ChannelNbRep <= 15 ) )
            {
                LoRaMacParams.ChannelsNbRep = mibSet->Param.ChannelNbRep;
            }
            else
            {
                status = LORAMAC_STATUS_PARAMETER_INVALID;
            }
            break;
        }
        case MIB_MAX_RX_WINDOW_DURATION:
        {
            LoRaMacParams.MaxRxWindow = mibSet->Param.MaxRxWindow;
            break;
        }
        case MIB_RECEIVE_DELAY_1:
        {
            LoRaMacParams.ReceiveDelay1 = mibSet->Param.ReceiveDelay1;
            break;
        }
        case MIB_RECEIVE_DELAY_2:
        {
            LoRaMacParams.ReceiveDelay2 = mibSet->Param.ReceiveDelay2;
            break;
        }
        case MIB_JOIN_ACCEPT_DELAY_1:
        {
            LoRaMacParams.JoinAcceptDelay1 = mibSet->Param.JoinAcceptDelay1;
            break;
        }
        case MIB_JOIN_ACCEPT_DELAY_2:
        {
            LoRaMacParams.JoinAcceptDelay2 = mibSet->Param.JoinAcceptDelay2;
            break;
        }
        case MIB_CHANNELS_DEFAULT_DATARATE:
        {
            verify.DatarateParams.Datarate = mibSet->Param.ChannelsDefaultDatarate;

            if(lora_phy->verify(&verify, PHY_DEF_TX_DR) == true)
            {
                LoRaMacParamsDefaults.ChannelsDatarate = verify.DatarateParams.Datarate;
            }
            else
            {
                status = LORAMAC_STATUS_PARAMETER_INVALID;
            }
            break;
        }
        case MIB_CHANNELS_DATARATE:
        {
            verify.DatarateParams.Datarate = mibSet->Param.ChannelsDatarate;
            verify.DatarateParams.UplinkDwellTime = LoRaMacParams.UplinkDwellTime;

            if(lora_phy->verify(&verify, PHY_TX_DR) == true)
            {
                LoRaMacParams.ChannelsDatarate = verify.DatarateParams.Datarate;
            }
            else
            {
                status = LORAMAC_STATUS_PARAMETER_INVALID;
            }
            break;
        }
        case MIB_CHANNELS_DEFAULT_TX_POWER:
        {
            verify.TxPower = mibSet->Param.ChannelsDefaultTxPower;

            if(lora_phy->verify(&verify, PHY_DEF_TX_POWER) == true)
            {
                LoRaMacParamsDefaults.ChannelsTxPower = verify.TxPower;
            }
            else
            {
                status = LORAMAC_STATUS_PARAMETER_INVALID;
            }
            break;
        }
        case MIB_CHANNELS_TX_POWER:
        {
            verify.TxPower = mibSet->Param.ChannelsTxPower;

            if(lora_phy->verify(&verify, PHY_TX_POWER) == true)
            {
                LoRaMacParams.ChannelsTxPower = verify.TxPower;
            }
            else
            {
                status = LORAMAC_STATUS_PARAMETER_INVALID;
            }
            break;
        }
        case MIB_UPLINK_COUNTER:
        {
            UpLinkCounter = mibSet->Param.UpLinkCounter;
            break;
        }
        case MIB_DOWNLINK_COUNTER:
        {
            DownLinkCounter = mibSet->Param.DownLinkCounter;
            break;
        }
        case MIB_SYSTEM_MAX_RX_ERROR:
        {
            LoRaMacParams.SystemMaxRxError = LoRaMacParamsDefaults.SystemMaxRxError = mibSet->Param.SystemMaxRxError;
            break;
        }
        case MIB_MIN_RX_SYMBOLS:
        {
            LoRaMacParams.MinRxSymbols = LoRaMacParamsDefaults.MinRxSymbols = mibSet->Param.MinRxSymbols;
            break;
        }
        case MIB_ANTENNA_GAIN:
        {
            LoRaMacParams.AntennaGain = mibSet->Param.AntennaGain;
            break;
        }
        default:
            status = LORAMAC_STATUS_SERVICE_UNKNOWN;
            break;
    }

    return status;
}

LoRaMacStatus_t LoRaMac::LoRaMacChannelAdd( uint8_t id, ChannelParams_t params )
{
    ChannelAddParams_t channelAdd;

    // Validate if the MAC is in a correct state
    if( ( LoRaMacState & LORAMAC_TX_RUNNING ) == LORAMAC_TX_RUNNING )
    {
        if( ( LoRaMacState & LORAMAC_TX_CONFIG ) != LORAMAC_TX_CONFIG )
        {
            return LORAMAC_STATUS_BUSY;
        }
    }

    channelAdd.NewChannel = &params;
    channelAdd.ChannelId = id;

    return lora_phy->add_channel(&channelAdd);
}

LoRaMacStatus_t LoRaMac::LoRaMacChannelRemove( uint8_t id )
{
    ChannelRemoveParams_t channelRemove;

    if( ( LoRaMacState & LORAMAC_TX_RUNNING ) == LORAMAC_TX_RUNNING )
    {
        if( ( LoRaMacState & LORAMAC_TX_CONFIG ) != LORAMAC_TX_CONFIG )
        {
            return LORAMAC_STATUS_BUSY;
        }
    }

    channelRemove.ChannelId = id;

    if(lora_phy->remove_channel(&channelRemove) == false)
    {
        return LORAMAC_STATUS_PARAMETER_INVALID;
    }

    lora_phy->put_radio_to_sleep();

    return LORAMAC_STATUS_OK;
}

LoRaMacStatus_t LoRaMac::LoRaMacMulticastChannelLink( MulticastParams_t *channelParam )
{
    if( channelParam == NULL )
    {
        return LORAMAC_STATUS_PARAMETER_INVALID;
    }
    if( ( LoRaMacState & LORAMAC_TX_RUNNING ) == LORAMAC_TX_RUNNING )
    {
        return LORAMAC_STATUS_BUSY;
    }

    // Reset downlink counter
    channelParam->DownLinkCounter = 0;

    if( MulticastChannels == NULL )
    {
        // New node is the fist element
        MulticastChannels = channelParam;
    }
    else
    {
        MulticastParams_t *cur = MulticastChannels;

        // Search the last node in the list
        while( cur->Next != NULL )
        {
            cur = cur->Next;
        }
        // This function always finds the last node
        cur->Next = channelParam;
    }

    return LORAMAC_STATUS_OK;
}

LoRaMacStatus_t LoRaMac::LoRaMacMulticastChannelUnlink( MulticastParams_t *channelParam )
{
    if( channelParam == NULL )
    {
        return LORAMAC_STATUS_PARAMETER_INVALID;
    }
    if( ( LoRaMacState & LORAMAC_TX_RUNNING ) == LORAMAC_TX_RUNNING )
    {
        return LORAMAC_STATUS_BUSY;
    }

    if( MulticastChannels != NULL )
    {
        if( MulticastChannels == channelParam )
        {
          // First element
          MulticastChannels = channelParam->Next;
        }
        else
        {
            MulticastParams_t *cur = MulticastChannels;

            // Search the node in the list
            while( cur->Next && cur->Next != channelParam )
            {
                cur = cur->Next;
            }
            // If we found the node, remove it
            if( cur->Next )
            {
                cur->Next = channelParam->Next;
            }
        }
        channelParam->Next = NULL;
    }

    return LORAMAC_STATUS_OK;
}

LoRaMacStatus_t LoRaMac::LoRaMacMlmeRequest( MlmeReq_t *mlmeRequest )
{
    LoRaMacStatus_t status = LORAMAC_STATUS_SERVICE_UNKNOWN;
    LoRaMacHeader_t macHdr;
    AlternateDrParams_t altDr;
    VerifyParams_t verify;
    GetPhyParams_t getPhy;
    PhyParam_t phyParam;

    if( mlmeRequest == NULL )
    {
        return LORAMAC_STATUS_PARAMETER_INVALID;
    }
    if( LoRaMacState != LORAMAC_IDLE )
    {
        return LORAMAC_STATUS_BUSY;
    }

    memset( ( uint8_t* ) &MlmeConfirm, 0, sizeof( MlmeConfirm ) );

    MlmeConfirm.Status = LORAMAC_EVENT_INFO_STATUS_ERROR;

    switch( mlmeRequest->Type )
    {
        case MLME_JOIN:
        {
            if( ( LoRaMacState & LORAMAC_TX_DELAYED ) == LORAMAC_TX_DELAYED )
            {
                return LORAMAC_STATUS_BUSY;
            }

            if( ( mlmeRequest->Req.Join.DevEui == NULL ) ||
                ( mlmeRequest->Req.Join.AppEui == NULL ) ||
                ( mlmeRequest->Req.Join.AppKey == NULL ) ||
                ( mlmeRequest->Req.Join.NbTrials == 0 ) )
            {
                return LORAMAC_STATUS_PARAMETER_INVALID;
            }

            // Verify the parameter NbTrials for the join procedure
            verify.NbJoinTrials = mlmeRequest->Req.Join.NbTrials;

            if(lora_phy->verify(&verify, PHY_NB_JOIN_TRIALS) == false)
            {
                // Value not supported, get default
                getPhy.Attribute = PHY_DEF_NB_JOIN_TRIALS;
                phyParam = lora_phy->get_phy_params( &getPhy );
                mlmeRequest->Req.Join.NbTrials = ( uint8_t ) phyParam.Value;
            }

            LoRaMacFlags.Bits.MlmeReq = 1;
            MlmeConfirm.MlmeRequest = mlmeRequest->Type;

            LoRaMacDevEui = mlmeRequest->Req.Join.DevEui;
            LoRaMacAppEui = mlmeRequest->Req.Join.AppEui;
            LoRaMacAppKey = mlmeRequest->Req.Join.AppKey;
            MaxJoinRequestTrials = mlmeRequest->Req.Join.NbTrials;

            // Reset variable JoinRequestTrials
            JoinRequestTrials = 0;

            // Setup header information
            macHdr.Value = 0;
            macHdr.Bits.MType  = FRAME_TYPE_JOIN_REQ;

            ResetMacParameters( );

            altDr.NbTrials = JoinRequestTrials + 1;

            LoRaMacParams.ChannelsDatarate = lora_phy->get_alternate_DR(&altDr);

            status = Send( &macHdr, 0, NULL, 0 );
            break;
        }
        case MLME_LINK_CHECK:
        {
            LoRaMacFlags.Bits.MlmeReq = 1;
            // LoRaMac will send this command piggy-backed
            MlmeConfirm.MlmeRequest = mlmeRequest->Type;

            status = mac_commands.AddMacCommand( MOTE_MAC_LINK_CHECK_REQ, 0, 0 );
            break;
        }
        case MLME_TXCW:
        {
            MlmeConfirm.MlmeRequest = mlmeRequest->Type;
            LoRaMacFlags.Bits.MlmeReq = 1;
            status = SetTxContinuousWave( mlmeRequest->Req.TxCw.Timeout );
            break;
        }
        case MLME_TXCW_1:
        {
            MlmeConfirm.MlmeRequest = mlmeRequest->Type;
            LoRaMacFlags.Bits.MlmeReq = 1;
            status = SetTxContinuousWave1( mlmeRequest->Req.TxCw.Timeout, mlmeRequest->Req.TxCw.Frequency, mlmeRequest->Req.TxCw.Power );
            break;
        }
        default:
            break;
    }

    if( status != LORAMAC_STATUS_OK )
    {
        NodeAckRequested = false;
        LoRaMacFlags.Bits.MlmeReq = 0;
    }

    return status;
}

LoRaMacStatus_t LoRaMac::LoRaMacMcpsRequest( McpsReq_t *mcpsRequest )
{
    GetPhyParams_t getPhy;
    PhyParam_t phyParam;
    LoRaMacStatus_t status = LORAMAC_STATUS_SERVICE_UNKNOWN;
    LoRaMacHeader_t macHdr;
    VerifyParams_t verify;
    uint8_t fPort = 0;
    void *fBuffer;
    uint16_t fBufferSize;
    int8_t datarate = DR_0;
    bool readyToSend = false;

    if( mcpsRequest == NULL )
    {
        return LORAMAC_STATUS_PARAMETER_INVALID;
    }
    if( LoRaMacState != LORAMAC_IDLE )
    {
        return LORAMAC_STATUS_BUSY;
    }

    macHdr.Value = 0;
    memset ( ( uint8_t* ) &McpsConfirm, 0, sizeof( McpsConfirm ) );
    McpsConfirm.Status = LORAMAC_EVENT_INFO_STATUS_ERROR;

    // AckTimeoutRetriesCounter must be reset every time a new request (unconfirmed or confirmed) is performed.
    AckTimeoutRetriesCounter = 1;

    switch( mcpsRequest->Type )
    {
        case MCPS_UNCONFIRMED:
        {
            readyToSend = true;
            AckTimeoutRetries = 1;

            macHdr.Bits.MType = FRAME_TYPE_DATA_UNCONFIRMED_UP;
            fPort = mcpsRequest->Req.Unconfirmed.fPort;
            fBuffer = mcpsRequest->Req.Unconfirmed.fBuffer;
            fBufferSize = mcpsRequest->Req.Unconfirmed.fBufferSize;
            datarate = mcpsRequest->Req.Unconfirmed.Datarate;
            break;
        }
        case MCPS_CONFIRMED:
        {
            readyToSend = true;
            AckTimeoutRetries = mcpsRequest->Req.Confirmed.NbTrials;

            macHdr.Bits.MType = FRAME_TYPE_DATA_CONFIRMED_UP;
            fPort = mcpsRequest->Req.Confirmed.fPort;
            fBuffer = mcpsRequest->Req.Confirmed.fBuffer;
            fBufferSize = mcpsRequest->Req.Confirmed.fBufferSize;
            datarate = mcpsRequest->Req.Confirmed.Datarate;
            break;
        }
        case MCPS_PROPRIETARY:
        {
            readyToSend = true;
            AckTimeoutRetries = 1;

            macHdr.Bits.MType = FRAME_TYPE_PROPRIETARY;
            fBuffer = mcpsRequest->Req.Proprietary.fBuffer;
            fBufferSize = mcpsRequest->Req.Proprietary.fBufferSize;
            datarate = mcpsRequest->Req.Proprietary.Datarate;
            break;
        }
        default:
            break;
    }

    // Filter fPorts
    // TODO: Does not work with PROPRIETARY messages
//    if( IsFPortAllowed( fPort ) == false )
//    {
//        return LORAMAC_STATUS_PARAMETER_INVALID;
//    }

    // Get the minimum possible datarate
    getPhy.Attribute = PHY_MIN_TX_DR;
    getPhy.UplinkDwellTime = LoRaMacParams.UplinkDwellTime;
    phyParam = lora_phy->get_phy_params( &getPhy );
    // Apply the minimum possible datarate.
    // Some regions have limitations for the minimum datarate.
    datarate = MAX( datarate, phyParam.Value );

    if( readyToSend == true )
    {
        if( LoRaMacParams.AdrCtrlOn == false )
        {
            verify.DatarateParams.Datarate = datarate;
            verify.DatarateParams.UplinkDwellTime = LoRaMacParams.UplinkDwellTime;

            if(lora_phy->verify(&verify, PHY_TX_DR) == true)
            {
                LoRaMacParams.ChannelsDatarate = verify.DatarateParams.Datarate;
            }
            else
            {
                return LORAMAC_STATUS_PARAMETER_INVALID;
            }
        }

        status = Send( &macHdr, fPort, fBuffer, fBufferSize );
        if( status == LORAMAC_STATUS_OK )
        {
            McpsConfirm.McpsRequest = mcpsRequest->Type;
            LoRaMacFlags.Bits.McpsReq = 1;
        }
        else
        {
            NodeAckRequested = false;
        }
    }

    return status;
}

radio_events_t *LoRaMac::GetPhyEventHandlers()
{
    RadioEvents.tx_done = mbed::callback(this, &LoRaMac::handle_tx_done);
    RadioEvents.rx_done = mbed::callback(this, &LoRaMac::handle_rx_done);
    RadioEvents.rx_error = mbed::callback(this, &LoRaMac::handle_rx_error);
    RadioEvents.tx_timeout = mbed::callback(this, &LoRaMac::handle_tx_timeout);
    RadioEvents.rx_timeout = mbed::callback(this, &LoRaMac::handle_rx_timeout);

    return &RadioEvents;
}

#if defined(LORAWAN_COMPLIANCE_TEST)
/***************************************************************************
 * Compliance testing                                                      *
 **************************************************************************/

LoRaMacStatus_t LoRaMac::LoRaMacSetTxTimer( uint32_t TxDutyCycleTime )
{
    _lora_time.TimerSetValue(&TxNextPacketTimer, TxDutyCycleTime);
    _lora_time.TimerStart(&TxNextPacketTimer);

    return LORAMAC_STATUS_OK;
}

LoRaMacStatus_t LoRaMac::LoRaMacStopTxTimer( )
{
    _lora_time.TimerStop(&TxNextPacketTimer);

    return LORAMAC_STATUS_OK;
}

void LoRaMac::LoRaMacTestRxWindowsOn( bool enable )
{
    IsRxWindowsEnabled = enable;
}

void LoRaMac::LoRaMacTestSetMic( uint16_t txPacketCounter )
{
    UpLinkCounter = txPacketCounter;
    IsUpLinkCounterFixed = true;
}

void LoRaMac::LoRaMacTestSetDutyCycleOn( bool enable )
{
    VerifyParams_t verify;

    verify.DutyCycle = enable;

    if(lora_phy->verify(&verify, PHY_DUTY_CYCLE) == true)
    {
        DutyCycleOn = enable;
    }
}

void LoRaMac::LoRaMacTestSetChannel( uint8_t channel )
{
    Channel = channel;
}
#endif