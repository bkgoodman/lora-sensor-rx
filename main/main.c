#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "neopixel.h"

#include "esp_log.h"
//#include "esp_heap_trace.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_system.h"

#include "ssd1306.h"
//#include "font8x8_basic.h"

#include "lora.h"

#if 0
#define _I2C_NUMBER(num) I2C_NUM_##num
#define I2C_NUMBER(num) _I2C_NUMBER(num)
#define OLED_I2C_MASTER_SCL_IO (CONFIG_OLED_I2C_MASTER_SCL)               /*!< gpio number for I2C master clock */
#define OLED_I2C_MASTER_SDA_IO (CONFIG_OLED_I2C_MASTER_SDA)               /*!< gpio number for I2C master data  */
#define OLED_I2C_MASTER_NUM (I2C_NUMBER(CONFIG_OLED_I2C_MASTER_PORT_NUM)) /*!< I2C port number for master dev */
#endif
#define NEOPIXEL_RMT_CHANNEL (0)
#define NEOPIXEL_WS2812

uint32_t		pixels[CONFIG_NEOPIXEL_COUNT];
pixel_settings_t px;

uint64_t last_seq_time=0;
unsigned short last_seq_val=0;
uint64_t last_pump1_time=0;
unsigned short last_pump1_val=0;
uint64_t last_pump2_time=0;
unsigned short last_pump2_val=0;

uint16_t crc16(const uint8_t *data, int length);
void set_pixels(uint16_t r, uint16_t g, uint16_t b, uint16_t l) {
  int i;
  for (i=0;i<CONFIG_NEOPIXEL_COUNT;i++) {
    np_set_pixel_rgbw_level(&px, i , r,g,b,0,l);
  }
  np_show(&px, NEOPIXEL_RMT_CHANNEL);
}


#define TAG "BKGLoRa"
static xQueueHandle evt_queue = NULL;
TimerHandle_t timer;
uint8_t mac[6];
char macstr[18];
int seq=0;
unsigned short pump1=0;
unsigned short pump2=0;
SSD1306_t dev;


void updateDisplay(char *line);
void updateSequence();

void init_leds() {
	int rc;
	rc = neopixel_init(CONFIG_NEOPIXEL_GPIO, NEOPIXEL_RMT_CHANNEL);
	//rc = neopixel_init(13, 0);
	if (rc < 0)
		ESP_LOGE(TAG, "neopixel_init rc = %d", rc);


	ESP_LOGE(TAG,"NEOPIXEL_COUNT %d",CONFIG_NEOPIXEL_COUNT);
	px.pixels = (uint8_t *)pixels;
	px.pixel_count = CONFIG_NEOPIXEL_COUNT;
#ifdef	NEOPIXEL_WS2812
	strcpy(px.color_order, "GRB");
#else
	strcpy(px.color_order, "GRBW");
#endif

	memset(&px.timings, 0, sizeof(px.timings));
	px.timings.mark.level0 = 1;
	px.timings.space.level0 = 1;
	px.timings.mark.duration0 = 12;
#ifdef	NEOPIXEL_WS2812
	px.nbits = 24;
	px.timings.mark.duration1 = 14;
	px.timings.space.duration0 = 7;
	px.timings.space.duration1 = 16;
	px.timings.reset.duration0 = 600;
	px.timings.reset.duration1 = 600;
#endif
#ifdef	NEOPIXEL_SK6812
	px.nbits = 32;
	px.timings.mark.duration1 = 12;
	px.timings.space.duration0 = 6;
	px.timings.space.duration1 = 18;
	px.timings.reset.duration0 = 900;
	px.timings.reset.duration1 = 900;
#endif
	px.brightness = 0x80;
	np_show(&px, NEOPIXEL_RMT_CHANNEL);

	set_pixels(64,64,0,255);
	vTaskDelay(500 / portTICK_PERIOD_MS);
	
	set_pixels( 128,128,0,255);
	vTaskDelay(500 / portTICK_PERIOD_MS);

	set_pixels( 0,0,0,255);
      ESP_LOGI(TAG,"Pixel init end...");
}

void task_tx(void *p,int txlen)
{
      ESP_LOGI(TAG,"Send packet...");
      lora_send_packet((uint8_t *)p,txlen);
	lora_receive();    // put into receive mode
      ESP_LOGI(TAG,"Sent.");
      updateSequence();
}

#define EVENT_LORA_RX 0
#define EVENT_TIMER 1
#define EVENT_SENSOR_1 2
#define EVENT_SENSOR_2  3
#define EVENT_UNKNOWN  99

// K1BKG 00:11:22:33:44:55:66 Message<CS>
// 0         1         2        3
// 0123456789-123456789-12345679-1234
int do_rx(char *p,int maxlen)
{
   int x;
   lora_receive();    // put into receive mode
   if(lora_received()) {
    x = lora_receive_packet((uint8_t *)p, maxlen-1);
    p[x] = 0;
    ESP_LOGI(TAG,"Received: (%d) \"%s\"",x, p);
    updateDisplay(p);
   lora_receive();    // put into receive mode
   if ((x >= 11) && !strncmp(p,"K1BKG ZERO!",11)) {
      seq=0;
      pump1=0;
      pump2=0;
    }
   else if ((x >= 11) && !strncmp(p,"K1BKG PING!",11)) {
      uint32_t item = EVENT_TIMER;
      xQueueSendFromISR(evt_queue, &item, NULL);
    }
   else if ((x >= 9) && !strncmp(p,"K1BKG ",5)) {
     char callsign[10];
    unsigned short newcrc;
     unsigned int r_seq,r_pump1,r_pump2,r_crc;
     int res = sscanf(p,"%8s GOT %x %x %x EOT %x",
         callsign,
         &r_seq,
         &r_pump1,
         &r_pump2,
         &r_crc);
     newcrc = crc16((const uint8_t *)p,x-4);
    ESP_LOGI(TAG,"Parsed (%d) %s %d %d %d CRC=0x%x/0x%x",res,callsign,r_seq,r_pump1,r_pump2,r_crc,newcrc);
    if (newcrc == r_crc) {
      uint64_t now = esp_timer_get_time();
      if (r_pump1 != last_pump1_val) {
        last_pump1_val = r_pump1;
        last_pump1_time = now;
      }
      if (r_pump2 != last_pump2_val) {
        last_pump2_val = r_pump2;
        last_pump2_time = now;
      }
      if (r_seq != last_seq_val) {
        last_seq_val = r_seq;
        last_seq_time = now;
      }
    }
   }
    return (x);
   }
   return(0);

}

int led_task(void *p) {
  unsigned short seq=0;
  while (1) {
    int i;
      vTaskDelay(100 / portTICK_PERIOD_MS);
      seq++;
      for (i=0;i<CONFIG_NEOPIXEL_COUNT;i++) {
         np_set_pixel_rgbw_level(&px, i , 32,0,0,0,255);
      }
      np_set_pixel_rgbw_level(&px, seq%8 , 0,255,0,0,255);
      np_show(&px, NEOPIXEL_RMT_CHANNEL);
  }
}

uint64_t sensor_debounce[2] = {0,0};

static void IRAM_ATTR lora_isr_handler(void* arg) {
    uint32_t item = EVENT_LORA_RX;
    xQueueSendFromISR(evt_queue, &item, NULL);
}

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t item=EVENT_UNKNOWN;
    if ((CONFIG_SENSOR_PIN_1) && (arg == (void *) CONFIG_SENSOR_PIN_1)) item=EVENT_SENSOR_1;
    if ((CONFIG_SENSOR_PIN_2) && (arg == (void *) CONFIG_SENSOR_PIN_2)) item=EVENT_SENSOR_2;

    uint64_t now = esp_timer_get_time();
    if ((item != EVENT_UNKNOWN) && (now > sensor_debounce[item - EVENT_SENSOR_1])) {

      sensor_debounce[item - EVENT_SENSOR_1] = now + 1000000;
      xQueueSendFromISR(evt_queue, &item, NULL);
    }
}
void LoRaTimer(TimerHandle_t xTimer) {
     ESP_LOGI(TAG,"LoRa Timer 0x%d\r",(uint32_t) xTimer);
    uint32_t item = EVENT_TIMER;
    xQueueSendFromISR(evt_queue, &item, NULL);
}

unsigned char buf[55];
void task_rx(void *p)
{
   int x;
   for(;;) {
      lora_receive();    // put into receive mode
      while(lora_received()) {
	      printf("RCVd...\n");
         x = lora_receive_packet(buf, sizeof(buf));
	 ESP_LOGW(TAG,"Post-Receive");
         buf[x] = 0;
         printf("Received: %s\n", buf);
         lora_receive();
      vTaskDelay(100 / portTICK_PERIOD_MS);
      }
      vTaskDelay(100 / portTICK_PERIOD_MS);
   }
}

#define CRC16_POLY 0x1021

uint16_t crc16(const uint8_t *data, int length) {
    uint16_t crc = 0xFFFF;

    for (int i = 0; i < length; i++) {
        crc ^= data[i] << 8;

        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ CRC16_POLY;
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}
static void main_task(void* arg)
{
    char rxbuf[128];
    uint32_t evtNo;
    short l;
    lora_receive();    // put into receive mode
    ESP_LOGI(TAG,"Main Task Started");
    for(;;) {
        if(xQueueReceive(evt_queue, &evtNo, portMAX_DELAY)) {
        ESP_LOGI(TAG,"Got Event Number %d",evtNo);
        switch (evtNo) {
          case EVENT_LORA_RX:
            do_rx(rxbuf,sizeof(rxbuf));
            break;
          case EVENT_TIMER:
            do_rx(rxbuf,sizeof(rxbuf)); // Why??? 
            //snprintf(rxbuf,sizeof(rxbuf),"%s %s %d is on the air",CONFIG_CALLSIGN,macstr,seq++);
            //snprintf(rxbuf,sizeof(rxbuf),"%s GOT %d %d %d EOT",CONFIG_CALLSIGN,seq++,pump1,pump2);
            l = snprintf(rxbuf,sizeof(rxbuf),"%s GOT %2.2x %2.2x %2.2x EOT ",CONFIG_CALLSIGN,seq++,pump1,pump2);
            snprintf(&rxbuf[l],sizeof(rxbuf)-l,"%4.4x",crc16((const uint8_t *)rxbuf,l));
            task_tx(rxbuf,strlen(rxbuf));
            ESP_LOGI(TAG,"Transmitted: %s",rxbuf);
            break;
          case EVENT_SENSOR_1:
            pump1++;
            updateSequence();
            break;
          case EVENT_SENSOR_2:
            pump2++;
            updateSequence();
            break;
          default:
            ESP_LOGE(TAG,"Unkown Event: %d",evtNo);
        }

      }
    }
}

void updateSequence() {
	char lineChar[32];
	ssd1306_clear_line(&dev, 2, true);
	snprintf(lineChar,sizeof(lineChar),"%d %d %d",seq,pump1,pump2);
	ssd1306_display_text(&dev, 2, lineChar, strlen(lineChar), true);
}

void updateDisplay(char *r) {
	int i;
	char dispstr[20];

	snprintf(dispstr,sizeof(dispstr),"RSSI:%d SNR:%3.3f",lora_packet_rssi(),lora_packet_snr());
	ssd1306_display_text(&dev, 3, dispstr, strlen(dispstr)>16? 16:strlen(r), false);

	for (i=0;(i<4) && (strlen(r));i++) {
		ssd1306_display_text(&dev, i+4, r, strlen(r)>16? 16:strlen(r), false);
		r+=16;
	}

}

void app_main()
{
	char lineChar[32];

   esp_read_mac((uint8_t *) &mac,ESP_MAC_ETH);
   snprintf(macstr,sizeof(macstr),"%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x",
		   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
   ESP_LOGI(TAG,"Mac: %s",macstr);

	ESP_LOGI(TAG,"OLED SDA %d OLED SCL %d MASTER_NUM %d",CONFIG_OLED_I2C_MASTER_SDA,CONFIG_OLED_I2C_MASTER_SCL,CONFIG_OLED_I2C_MASTER_PORT_NUM);
	//i2c_master_init(&dev, CONFIG_OLED_I2C_MASTER_SDA, CONFIG_OLED_I2C_MASTER_SCL, -1);
	i2c_master_init(&dev, 21, 22, -1);
	ssd1306_init(&dev, 128, 64);
 	ESP_LOGI(TAG, "Panel is 128x64");
  //dev._flip=1;
	ssd1306_clear_screen(&dev, false);
	ssd1306_contrast(&dev, 0xff);
	ssd1306_clear_line(&dev, 0, true);
	ssd1306_clear_line(&dev, 1, true);
	ssd1306_clear_line(&dev, 2, true);
	ssd1306_display_text(&dev, 0, CONFIG_CALLSIGN, strlen(CONFIG_CALLSIGN), true);
	ssd1306_display_text(&dev, 1, &macstr[3], strlen(macstr)-3, true);
	snprintf(lineChar,sizeof(lineChar),"Sequence: %d",seq);
	ssd1306_display_text(&dev, 2, lineChar, strlen(lineChar), true);
	/*
	ssd1306_display_text(&dev, 0, "SSD1306 128x64", 14, false);
	ssd1306_display_text(&dev, 1, "ABCDEFGHIJKLMNOP", 16, false);
	ssd1306_display_text(&dev, 2, "abcdefghijklmnop",16, false);
	ssd1306_display_text(&dev, 3, "Hello World!!", 13, false);
	ssd1306_clear_line(&dev, 4, true);
	ssd1306_clear_line(&dev, 5, true);
	ssd1306_clear_line(&dev, 6, true);
	ssd1306_clear_line(&dev, 7, true);
	ssd1306_display_text(&dev, 4, "SSD1306 128x64", 14, true);
	ssd1306_display_text(&dev, 5, "ABCDEFGHIJKLMNOP", 16, true);
	ssd1306_display_text(&dev, 6, "abcdefghijklmnop",16, true);
	ssd1306_display_text(&dev, 7, "Hello World!!", 13, true);
	*/
   evt_queue = xQueueCreate(10, sizeof(uint32_t));

   // BKG REMOVED gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3);

   // Set Button handler 
   gpio_config_t io_conf;
   io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
   io_conf.pin_bit_mask = (1<< CONFIG_IRQ_GPIO);
   io_conf.mode = GPIO_MODE_INPUT;
   io_conf.pull_up_en = 1;
   gpio_config(&io_conf);
   gpio_isr_handler_add(CONFIG_IRQ_GPIO, lora_isr_handler, (void*) 0);

  // Setup input receivers
    io_conf.intr_type = GPIO_INTR_NEGEDGE; // Interrupt on positive edge
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;

    io_conf.pin_bit_mask = (1ULL << CONFIG_SENSOR_PIN_1);
    if (CONFIG_SENSOR_PIN_1) gpio_config(&io_conf);

    io_conf.pin_bit_mask = (1ULL << CONFIG_SENSOR_PIN_2);
    if (CONFIG_SENSOR_PIN_2) gpio_config(&io_conf);

    // Install the GPIO ISR service
    // gpio_install_isr_service(0);

    // Hook the ISR handler to both GPIO pins
    //if (CONFIG_SENSOR_PIN_1) gpio_isr_handler_add(CONFIG_SENSOR_PIN_1, gpio_isr_handler, (void*) CONFIG_SENSOR_PIN_1);
    //if (CONFIG_SENSOR_PIN_2) gpio_isr_handler_add(CONFIG_SENSOR_PIN_2, gpio_isr_handler, (void*) CONFIG_SENSOR_PIN_2);
  
   if (CONFIG_SENSOR_PIN_1) ESP_LOGI(TAG,"Sensor 1 on GPIO %d",CONFIG_SENSOR_PIN_1);
   if (CONFIG_SENSOR_PIN_2) ESP_LOGI(TAG,"Sensor 2 on GPIO %d",CONFIG_SENSOR_PIN_2);
   int err = lora_init();
   ESP_LOGI(TAG,"LoRa Init: %d",err);

   ESP_LOGI(TAG,"LoRa Check...");
   lora_initialized();
   ESP_LOGI(TAG,"LoRa SetFreq...");
   lora_set_frequency(433775000);
   lora_set_spreading_factor(12);
   lora_set_tx_power(17);
   lora_set_bandwidth(125000);
   lora_set_coding_rate(8);
   //ESP_LOGI(TAG,"LoRa Enable CRC...");
   //lora_enable_crc();
   ESP_LOGI(TAG,"LoRa Start TX Task...");
   xTaskCreate(&main_task, "main_task", 8192, NULL, 5, NULL);
   ESP_LOGI(TAG,"Main Created...");
   //xTaskCreate(&task_rx, "rx_tx", 8192, NULL, 5, NULL);
   //ESP_LOGI(TAG,"Rx Running...");
  

   //ESP_LOGI(TAG,"OLED SDA %d OLED SCL %d MASTER_NUM %d",OLED_I2C_MASTER_SDA_IO,OLED_I2C_MASTER_SCL_IO,OLED_I2C_MASTER_NUM);

   //task_tx("Testing",7);
   timer = xTimerCreate("Timer",(CONFIG_TRANSMIT_SECONDS*1000 / portTICK_PERIOD_MS ),pdTRUE,(void *) 0,LoRaTimer);
   ESP_LOGI(TAG,"Timer Created");
   xTimerStart(timer,0);
   ESP_LOGI(TAG,"Timer Started");
#if 0
#endif
   init_leds();
   xTaskCreate(&led_task, "led_task", 8192, NULL, 5, NULL);

}
