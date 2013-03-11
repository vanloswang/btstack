//*****************************************************************************
//
// spp_counter demo - it provides an SPP and sends a counter every second
//
// it doesn't use the LCD to get down to a minimal memory footprint
//
//*****************************************************************************

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <msp430x54x.h>

#include "bt_control_cc256x.h"
#include "hal_board.h"
#include "hal_compat.h"
#include "hal_usb.h"

#include <btstack/hci_cmds.h>
#include <btstack/run_loop.h>
#include <btstack/sdp_util.h>

#include "hci.h"
#include "l2cap.h"
#include "btstack_memory.h"
#include "remote_device_db.h"
#include "rfcomm.h"
#include "sdp.h"
#include "config.h"

#define HEARTBEAT_PERIOD_MS 1000

static uint8_t   rfcomm_channel_nr = 1;
static uint16_t  rfcomm_channel_id = 0;
static uint8_t   spp_service_buffer[100];
static timer_source_t heartbeat;
    
static int real_counter = 0;
static int counter_to_send = 0;

enum STATE {INIT, W4_CONNECTION, W4_CHANNEL_COMPLETE, ACTIVE} ;
enum STATE state = INIT;

static void tryToSend(void){
    if (!rfcomm_channel_id) return;
    if (real_counter < counter_to_send) return;
                
    char lineBuffer[30];
    sprintf(lineBuffer, "BTstack counter %04u\n\r", counter_to_send);
    printf(lineBuffer);
    int err = rfcomm_send_internal(rfcomm_channel_id, (uint8_t*) lineBuffer, strlen(lineBuffer));

    switch (err){
        case 0:
            counter_to_send++;
            break;
        case BTSTACK_ACL_BUFFERS_FULL:
            break;
        default:
           printf("rfcomm_send_internal() -> err %d\n\r", err);
        break;
    }
}

// Bluetooth logic
static void packet_handler (void *context, uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    bd_addr_t event_addr;
    uint8_t   rfcomm_channel_nr;
    uint16_t  mtu;
    uint8_t event = packet[0];
    
    // printf("packet_handler: packettype: 0x%02x, packet[0]: 0x%02x, state %u\n", packet_type, packet[0], state);

    // handle events, ignore data
    if (packet_type != HCI_EVENT_PACKET) return;

    switch(state){
        case INIT:
            if (event != BTSTACK_EVENT_STATE) break;
            printf("INIT: bt stack activated, get started, hci.state=%u \r\n", packet[2]); 
            // bt stack activated, get started - set local name
            if (packet[2] == HCI_STATE_WORKING) {
                printf("INIT: set local name - BTstack SPP Counter\r\n");
                hci_send_cmd(&hci_write_local_name, "BTstack SPP Counter");
                state = W4_CONNECTION;
            }
            break;

        case W4_CONNECTION:
            switch (event) {
                case HCI_EVENT_PIN_CODE_REQUEST:
                    // inform about pin code request
                    printf("W4_CONNECTION: Pin code request - using '0000'\n\r");
                    bt_flip_addr(event_addr, &packet[2]);
                    hci_send_cmd(&hci_pin_code_request_reply, &event_addr, 4, "0000");
                    break;
                
                case RFCOMM_EVENT_INCOMING_CONNECTION:
                    // data: event (8), len(8), address(48), channel (8), rfcomm_cid (16)
                    bt_flip_addr(event_addr, &packet[2]); 
                    rfcomm_channel_nr = packet[8];
                    rfcomm_channel_id = READ_BT_16(packet, 9);
                    printf("W4_CONNECTION: RFCOMM channel %u requested for %s\n\r", rfcomm_channel_nr, bd_addr_to_str(event_addr));
                    rfcomm_accept_connection_internal(rfcomm_channel_id);
                    state = W4_CHANNEL_COMPLETE;
                    break;
                default:
                    break;
            }
        
        case W4_CHANNEL_COMPLETE:
                if ( event != RFCOMM_EVENT_OPEN_CHANNEL_COMPLETE ) break;
                
                // data: event(8), len(8), status (8), address (48), server channel(8), rfcomm_cid(16), max frame size(16)
                if (packet[2]) {
                    printf("W4_CHANNEL_COMPLETE: RFCOMM channel open failed, status %u\n\r", packet[2]);
                    break;
                } 
                
                rfcomm_channel_id = READ_BT_16(packet, 12);
                mtu = READ_BT_16(packet, 14);
                printf("\n\rW4_CHANNEL_COMPLETE: RFCOMM channel open succeeded. New RFCOMM Channel ID %u, max frame size %u\n\r", rfcomm_channel_id, mtu);
                state = ACTIVE;
                break;
        
        case ACTIVE:
            switch(event){
                case DAEMON_EVENT_HCI_PACKET_SENT:
                case RFCOMM_EVENT_CREDITS:
                    tryToSend();
                    break;
                case RFCOMM_EVENT_CHANNEL_CLOSED:
                    rfcomm_channel_id = 0;
                    state = W4_CONNECTION;
                    break;
                default:
                    break;
            }
        
        default:
            break;
    }
}

static void run_loop_register_timer(timer_source_t *timer, uint16_t period){
    run_loop_set_timer(timer, period);
    run_loop_add_timer(timer);
}

static void  heartbeat_handler(timer_source_t *ts){
    // re-register timer
    real_counter++;
    run_loop_register_timer(ts, HEARTBEAT_PERIOD_MS);
} 

static void timer_setup(){
    // set one-shot timer
    heartbeat.process = &heartbeat_handler;
    run_loop_register_timer(&heartbeat, HEARTBEAT_PERIOD_MS);
}


static void hw_setup(){
    // stop watchdog timer
    WDTCTL = WDTPW + WDTHOLD;

    //Initialize clock and peripherals 
    halBoardInit();  
    halBoardStartXT1(); 
    halBoardSetSystemClock(SYSCLK_16MHZ);
    
    // init debug UART
    halUsbInit();

    // init LEDs
    LED_PORT_OUT |= LED_1 | LED_2;
    LED_PORT_DIR |= LED_1 | LED_2;
}

static void btstack_setup(){
    /// GET STARTED with BTstack ///
    btstack_memory_init();
    run_loop_init(RUN_LOOP_EMBEDDED);
    
    // init HCI
    hci_transport_t    * transport = hci_transport_h4_dma_instance();
    bt_control_t       * control   = bt_control_cc256x_instance();
    hci_uart_config_t  * config    = hci_uart_config_cc256x_instance();
    remote_device_db_t * remote_db = (remote_device_db_t *) &remote_device_db_memory;
    hci_init(transport, config, control, remote_db);
    
    // use eHCILL
    bt_control_cc256x_enable_ehcill(1);
    
    // init L2CAP
    l2cap_init();
    l2cap_register_packet_handler(packet_handler);
    
    // init RFCOMM
    rfcomm_init();
    rfcomm_register_packet_handler(packet_handler);
    rfcomm_register_service_internal(NULL, rfcomm_channel_nr, 100);  // reserved channel, mtu=100

    // init SDP, create record for SPP and register with SDP
    sdp_init();
    memset(spp_service_buffer, 0, sizeof(spp_service_buffer));
    service_record_item_t * service_record_item = (service_record_item_t *) spp_service_buffer;
    sdp_create_spp_service( (uint8_t*) &service_record_item->service_record, 1, "SPP Counter");
    printf("SDP service buffer size: %u\n\r", (uint16_t) (sizeof(service_record_item_t) + de_get_len((uint8_t*) &service_record_item->service_record)));
    sdp_register_service_internal(NULL, service_record_item);
}


// main
int main(void){
    hw_setup();
    btstack_setup();
    timer_setup();

    // ready - enable irq used in h4 task
    __enable_interrupt();   
    
    printf("Run...\n\r");

    // turn on!
    hci_power_control(HCI_POWER_ON);
    // make discoverable
    hci_discoverable_control(1);
                    
    // go!
    run_loop_execute(); 
    
    // happy compiler!
    return 0;
}

