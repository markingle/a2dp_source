/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_app_trace.h"

#include "esp_bt.h"
#include "bt_app_core.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#include "esp_spiffs.h"
#include <fcntl.h>

//The following headers are for GPIO intregration and tasks background processes for interrupts
#include "c_timeutils.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "gpio_task.h"
#include "sdkconfig.h"
#include "freertos/event_groups.h"

//The following headers are for Pulse count integrtion to measure change in oscillator frequency
#include "driver/pcnt.h"

#define BT_AV_TAG               "BT_AV"
#define SPIFFS_TAG              "SPIFFS"

#define TABLE_SIZE_441HZ            100

#define COUNT_SAMPLE_SIZE                10

/* event for handler "bt_av_hdl_stack_up */
enum {
    BT_APP_EVT_STACK_UP = 0,
};

/* A2DP global state */
enum {
    APP_AV_STATE_IDLE,
    APP_AV_STATE_DISCOVERING,
    APP_AV_STATE_DISCOVERED,
    APP_AV_STATE_UNCONNECTED,
    APP_AV_STATE_CONNECTING,
    APP_AV_STATE_CONNECTED,
    APP_AV_STATE_DISCONNECTING,
};

/* sub states of APP_AV_STATE_CONNECTED */
enum {
    APP_AV_MEDIA_STATE_IDLE,
    APP_AV_MEDIA_STATE_STARTING,
    APP_AV_MEDIA_STATE_STARTED,
    APP_AV_MEDIA_STATE_STOPPING,
};

EventGroupHandle_t alarm_eventgroup;

xQueueHandle pcnt_evt_queue;   // A queue to handle pulse counter events
pcnt_isr_handle_t user_isr_handle = NULL; //user's ISR service handle

//static QueueHandle_t q1;

const int GPIO_SENSE_BIT = BIT0;

//GPIO pin assignments
#define BLINK_GPIO GPIO_NUM_2
#define TDA1606_GPIO GPIO_NUM_27
#define GPIO_TDA_INPUT  22
#define GPIO_GLOVE_OUTPUT_SWITCH 4
#define GPIO_OUTPUT    23
#define GPIO_INPUT     0

//Value settings to configure Pulse Count functions
#define PCNT_TEST_UNIT      PCNT_UNIT_7
#define GUESS PCNT
#define PCNT_H_LIM_VAL      10000
#define PCNT_L_LIM_VAL     -10
#define PCNT_THRESH1_VAL    5
#define PCNT_THRESH0_VAL   -5
#define PCNT_INPUT_SIG_IO   4  // Pulse Input GPIO
#define PCNT_INPUT_CTRL_IO  5  // Control GPIO HIGH=count up, LOW=count down
#define LEDC_OUTPUT_IO      18 // Output GPIO of a sample 1 Hz pulse generator

//Sound function for speaker
#define GPIO_OUTPUT_SPEED LEDC_HIGH_SPEED_MODE

#define BT_APP_HEART_BEAT_EVT                (0xff00)

/// handler for bluetooth stack enabled events
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);

/// callback function for A2DP source
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

/// callback function for A2DP source audio data stream
static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len);

static void a2d_app_heart_beat(void *arg);

/// A2DP application state machine
static void bt_app_av_sm_hdlr(uint16_t event, void *param);

/* A2DP application state machine handler for each state */
static void bt_app_av_state_unconnected(uint16_t event, void *param);
static void bt_app_av_state_connecting(uint16_t event, void *param);
static void bt_app_av_state_connected(uint16_t event, void *param);
static void bt_app_av_state_disconnecting(uint16_t event, void *param);

static esp_bd_addr_t peer_bda = {0};
static uint8_t peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
static int m_a2d_state = APP_AV_STATE_IDLE;
static int m_media_state = APP_AV_MEDIA_STATE_IDLE;
static int m_intv_cnt = 0;
static int m_connecting_intv = 0;
static uint32_t m_pkt_cnt = 0;

static int sine_phase, core;

TimerHandle_t tmr;
TaskHandle_t DetectMetal_Task;

int wavfile = 0;

static const int16_t sine_int16[] = {
     0,    2057,    4107,    6140,    8149,   10126,   12062,   13952,   15786,   17557,
 19260,   20886,   22431,   23886,   25247,   26509,   27666,   28714,   29648,   30466,
 31163,   31738,   32187,   32509,   32702,   32767,   32702,   32509,   32187,   31738,
 31163,   30466,   29648,   28714,   27666,   26509,   25247,   23886,   22431,   20886,
 19260,   17557,   15786,   13952,   12062,   10126,    8149,    6140,    4107,    2057,
     0,   -2057,   -4107,   -6140,   -8149,  -10126,  -12062,  -13952,  -15786,  -17557,
-19260,  -20886,  -22431,  -23886,  -25247,  -26509,  -27666,  -28714,  -29648,  -30466,
-31163,  -31738,  -32187,  -32509,  -32702,  -32767,  -32702,  -32509,  -32187,  -31738,
-31163,  -30466,  -29648,  -28714,  -27666,  -26509,  -25247,  -23886,  -22431,  -20886,
-19260,  -17557,  -15786,  -13952,  -12062,  -10126,   -8149,   -6140,   -4107,   -2057,
};

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

//void IRAM_ATTR gpio_isr_handler(void* arg) {
//    uint32_t gpio_num = (uint32_t) arg;
//    BaseType_t xHigherPriorityTaskWoken;
//    if (gpio_num==GPIO_OUTPUT) {
//        xEventGroupSetBitsFromISR(alarm_eventgroup, GPIO_SENSE_BIT, &xHigherPriorityTaskWoken);
//    }
//}

//static void handler(void *args) {
//    gpio_num_t gpio;
//    gpio = GPIO_TDA_INPUT;
//    xQueueSendToBackFromISR(q1, &gpio, NULL);
//    }

//Structure for the eight pulse units
typedef struct {
    int unit;  // the PCNT unit that originated an interrupt
    uint32_t status; // information on the event type that caused the interrupt
} pcnt_evt_t;

void sound(int gpio_num,uint32_t freq,uint32_t duration) {

    ledc_timer_config_t timer_conf;
    timer_conf.speed_mode = GPIO_OUTPUT_SPEED;
    timer_conf.bit_num    = LEDC_TIMER_10_BIT;
    timer_conf.timer_num  = LEDC_TIMER_0;
    timer_conf.freq_hz    = freq;
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ledc_conf;
    ledc_conf.gpio_num   = gpio_num;
    ledc_conf.speed_mode = GPIO_OUTPUT_SPEED;
    ledc_conf.channel    = LEDC_CHANNEL_0;
    ledc_conf.intr_type  = LEDC_INTR_DISABLE;
    ledc_conf.timer_sel  = LEDC_TIMER_0;
    ledc_conf.duty       = 0x0; // 50%=0x3FFF, 100%=0x7FFF for 15 Bit
                                // 50%=0x01FF, 100%=0x03FF for 10 Bit
    ledc_channel_config(&ledc_conf);

    // start
    ledc_set_duty(GPIO_OUTPUT_SPEED, LEDC_CHANNEL_0, 0x7F); // 12% duty - play here for your speaker or buzzer
    ledc_update_duty(GPIO_OUTPUT_SPEED, LEDC_CHANNEL_0);
    vTaskDelay(duration/portTICK_PERIOD_MS);
    // stop
    ledc_set_duty(GPIO_OUTPUT_SPEED, LEDC_CHANNEL_0, 0);
    ledc_update_duty(GPIO_OUTPUT_SPEED, LEDC_CHANNEL_0);

}

// based on https://wiki.mikrotik.com/wiki/Super_Mario_Theme
void beep() {
    sound(GPIO_OUTPUT,660,100);
}

/*static void gpio_task_example(void* arg)
{
    uint32_t io_num;
    ESP_LOGI(BT_AV_TAG, "HELO");
    for(;;) {
        if(xQueueReceive(q1, &io_num, portMAX_DELAY)) {
            printf("GPIO[%d] intr, val: %d\n", io_num, gpio_get_level(io_num));
        }
    }
}

static void IRAM_ATTR pcnt_example_intr_handler(void *arg)
{
    uint32_t intr_status = PCNT.int_st.val;
    int i;
    pcnt_evt_t evt;
    portBASE_TYPE HPTaskAwoken = pdFALSE;

    for (i = 0; i < PCNT_UNIT_MAX; i++) {
        if (intr_status & (BIT(i))) {
            evt.unit = i;
            //Save the PCNT event type that caused an interrupt to pass it to the main program
            evt.status = PCNT.status_unit[i].val;
            PCNT.int_clr.val = BIT(i);
            xQueueSendFromISR(pcnt_evt_queue, &evt, &HPTaskAwoken);
            if (HPTaskAwoken == pdTRUE) {
                portYIELD_FROM_ISR();
            }
        }
    }
}*/



/* Configure LED PWM Controller
 * to output sample pulses at 1 Hz with duty of about 10%
 */
static void ledc_init(void)
{
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer;
    ledc_timer.speed_mode       = LEDC_HIGH_SPEED_MODE;
    ledc_timer.timer_num        = LEDC_TIMER_1;
    ledc_timer.duty_resolution  = LEDC_TIMER_10_BIT;
    ledc_timer.freq_hz          = 1;  // set output frequency at 1 Hz
    ledc_timer_config(&ledc_timer);

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel;
    ledc_channel.speed_mode = LEDC_HIGH_SPEED_MODE;
    ledc_channel.channel    = LEDC_CHANNEL_1;
    ledc_channel.timer_sel  = LEDC_TIMER_1;
    ledc_channel.intr_type  = LEDC_INTR_DISABLE;
    ledc_channel.gpio_num   = LEDC_OUTPUT_IO;
    ledc_channel.duty       = 100; // set duty at about 10%
    ledc_channel.hpoint     = 0;
    ledc_channel_config(&ledc_channel);
}

/* Initialize PCNT functions:
 *  - configure and initialize PCNT
 *  - set up the input filter
 *  - set up the counter events to watch
 */
static void metaldetector_pcnt_init(void)
{
    /* Prepare configuration for the PCNT unit */
    pcnt_config_t pcnt_config = {
        // Set PCNT input signal and control GPIOs
        .pulse_gpio_num = PCNT_INPUT_SIG_IO,
        .ctrl_gpio_num = PCNT_INPUT_CTRL_IO,
        .channel = PCNT_CHANNEL_0,
        .unit = PCNT_TEST_UNIT,  //PCNT_TEST_UNIT
        // What to do on the positive / negative edge of pulse input?
        .pos_mode = PCNT_COUNT_INC,   // Count up on the positive edge
        .neg_mode = PCNT_COUNT_DIS,   // Keep the counter value on the negative edge
        // What to do when control input is low or high?
        .lctrl_mode = PCNT_MODE_KEEP, // Changing to PCNT_MODE_KEEP so that default is to count up on low.  //Reverse counting direction if low
        .hctrl_mode = PCNT_MODE_KEEP,    // Keep the primary counter mode if high
        // Set the maximum and minimum limit values to watch
        .counter_h_lim = PCNT_H_LIM_VAL,
        .counter_l_lim = PCNT_L_LIM_VAL,
    };
    /* Initialize PCNT unit */
    pcnt_unit_config(&pcnt_config);

    //<<<<<<<< NOT NEEDED.  LEAVING FOR REFERNCE. REMOVE BEFORE ALPHA RELEASE >>>>>>>>>>>>>
    /* Configure and enable the input filter */
    //pcnt_set_filter_value(PCNT_TEST_UNIT, 100);
    //pcnt_filter_enable(PCNT_TEST_UNIT);

    /* Set threshold 0 and 1 values and enable events to watch */
    //pcnt_set_event_value(PCNT_TEST_UNIT, PCNT_EVT_THRES_1, PCNT_THRESH1_VAL);
    //pcnt_event_enable(PCNT_TEST_UNIT, PCNT_EVT_THRES_1);
    //pcnt_set_event_value(PCNT_TEST_UNIT, PCNT_EVT_THRES_0, PCNT_THRESH0_VAL);
    //pcnt_event_enable(PCNT_TEST_UNIT, PCNT_EVT_THRES_0);
    
    /* Enable events on zero, maximum and minimum limit values */
    //pcnt_event_enable(PCNT_TEST_UNIT, PCNT_EVT_ZERO);
    //pcnt_event_enable(PCNT_TEST_UNIT, PCNT_EVT_H_LIM);
    //pcnt_event_enable(PCNT_TEST_UNIT, PCNT_EVT_L_LIM);
    //<<<<<<<< NOT NEEDED.  LEAVING FOR REFERNCE. REMOVE BEFORE ALPHA RELEASE >>>>>>>>>>>>>

    /* Initialize PCNT's counter */
    pcnt_counter_pause(PCNT_TEST_UNIT);
    pcnt_counter_clear(PCNT_TEST_UNIT);

    /* Register ISR handler and enable interrupts for PCNT unit */

    //<<<<We want to avoid using ISR if possible.  Commenting this out until we know its required!!! >>>>>>>
    //pcnt_isr_register(pcnt_example_intr_handler, NULL, 0, &user_isr_handle);
    //pcnt_intr_enable(PCNT_TEST_UNIT);

    /* Everything is set up, now go to counting */
    pcnt_counter_resume(PCNT_TEST_UNIT);
}

static void detect_metal()
{
    //Set count and create Pulse Count structure
    int16_t count, sum = 0, init_freq = 0;
    int16_t i, average_diff;
    int16_t first_count = 0;
    int16_t second_count = 0;
    int16_t startup_frequency = 0;
    int16_t oldvalue, running_frequency = 0;
    int16_t temp = 0;
    float count_difference[COUNT_SAMPLE_SIZE];

    while(1){
        if (init_freq==0){
            beep();
            printf("Determining frequency average on start up\n");
            for(i = 0; i < COUNT_SAMPLE_SIZE; ++i)
                {
                printf("Start calibration....pass #%d\n", i);
                pcnt_get_counter_value(PCNT_TEST_UNIT, &first_count);
                core = xPortGetCoreID();
                printf("Startup Calibration Pulse Counter is on - Core: %d\n", core);
                printf("First counter value for calibration:%d\n", first_count);
                vTaskDelay(150 / portTICK_PERIOD_MS); // 150 milliseconds
                pcnt_get_counter_value(PCNT_TEST_UNIT, &second_count);
                printf("Second counter value for calibration:%d\n", second_count);
                count_difference[i] = (second_count - first_count);
                printf("Count Difference: %lf\n", count_difference[i]);
                sum += count_difference[i];
                average_diff = sum/COUNT_SAMPLE_SIZE;
                }
            printf("SUM: %d\n", sum);
            printf("Average: %d\n", average_diff);
            init_freq = 1;  // init complete set to 1 to start detecting metal
            first_count = 0; // reuse first and second variables
            second_count = 0;
        } else{
            printf("Analyzing frequency data to detect metal....\n");
            pcnt_get_counter_value(PCNT_TEST_UNIT, &first_count);
            printf("Running first counter value :%d\n", first_count);
            vTaskDelay(150 / portTICK_PERIOD_MS); // 150 milliseconds
            pcnt_get_counter_value(PCNT_TEST_UNIT, &second_count);
            printf("Running second counter value :%d\n", second_count);
            // At some point there will be a counter overrun these lines calc the difference at the overrun and reset second_count
            if (second_count < first_count){
                printf("Overrun detected...resetting second_count\n");
                second_count=second_count+((first_count - PCNT_H_LIM_VAL)*-1);
                running_frequency = second_count;
            } else {
                running_frequency = (second_count - first_count);
            }
            
            printf("The frequnecy difference is :%d\n", running_frequency);
            if (running_frequency < (average_diff-1) || running_frequency > (average_diff+1)) { //Deadband logic
                printf("Frequency +++++++\n");
                beep();
                oldvalue = running_frequency;
            } else {
                printf("Frequency -------\n");
                oldvalue = running_frequency;
            }
        }
    }
}


void app_main()
{
    //gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    //gpio_set_level(BLINK_GPIO, 0);

    //******************************************* SETUP BLUETOOTH STACK GAP and Classic Bluetooth ***************************************

    esp_log_level_set("*", ESP_LOG_DEBUG);
    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
       ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT(); // @suppress("Symbol is not resolved")

    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s initialize controller failed\n", __func__);
        return;
    }

    if (esp_bt_controller_enable(ESP_BT_MODE_BTDM) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s enable controller failed\n", __func__);
        return;
    }

    if (esp_bluedroid_init() != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s initialize bluedroid failed\n", __func__);
        return;
    }

    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s enable bluedroid failed\n", __func__);
        return;
    }

    /* create application task */
    bt_app_task_start_up();

    /* Bluetooth device name, connection mode and profile set up */
    bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_EVT_STACK_UP, NULL, 0, NULL);

    //******************************************* SETUP SPIFFS FOR PCM FILE ***************************************
/*
    ESP_LOGI(SPIFFS_TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true
    };
    
    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    esp_err_t spfs_ret = esp_vfs_spiffs_register(&conf);

    if (spfs_ret != ESP_OK) {
        if (spfs_ret == ESP_FAIL) {
            ESP_LOGE(SPIFFS_TAG, "Failed to mount or format filesystem");
        } else if (spfs_ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(SPIFFS_TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(SPIFFS_TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(spfs_ret));
        }
        return(0);
    }
    
    size_t total = 0, used = 0;
    spfs_ret = esp_spiffs_info(NULL, &total, &used);
    if (spfs_ret != ESP_OK) {
        ESP_LOGE(SPIFFS_TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(spfs_ret));
    } else {
        ESP_LOGI(SPIFFS_TAG, "Partition size: total: %d, used: %d", total, used);
    }

    wavfile = open("/spiffs/SinWave_16bit.wav", O_RDONLY);
    if (wavfile < 0) {
        ESP_LOGI(SPIFFS_TAG, "Failed to open file for writing");
        return(1);
    }

    ESP_LOGI(SPIFFS_TAG, "File opened");

    struct stat fileStat;
    if(stat("/spiffs/WarningMetalDetected_16bit.wav",&fileStat) < 0)    
        return 1;
 
    printf("Information for %s\n", "/spiffs/WarningMetalDetected_16bit.wav");
    printf("---------------------------\n");
    printf("File Size: \t\t%ld bytes\n",fileStat.st_size);
    printf("Number of Links: \t%d\n",fileStat.st_nlink);
    printf("File inode: \t\t%d\n",fileStat.st_ino);
 
    printf("File Permissions: \t");
    printf( (S_ISDIR(fileStat.st_mode)) ? "d" : "-");
    printf( (fileStat.st_mode & S_IRUSR) ? "r" : "-");
    printf( (fileStat.st_mode & S_IWUSR) ? "w" : "-");
    printf( (fileStat.st_mode & S_IXUSR) ? "x" : "-");
    printf( (fileStat.st_mode & S_IRGRP) ? "r" : "-");
    printf( (fileStat.st_mode & S_IWGRP) ? "w" : "-");
    printf( (fileStat.st_mode & S_IXGRP) ? "x" : "-");
    printf( (fileStat.st_mode & S_IROTH) ? "r" : "-");
    printf( (fileStat.st_mode & S_IWOTH) ? "w" : "-");
    printf( (fileStat.st_mode & S_IXOTH) ? "x" : "-");
    printf("\n\n");
 
    printf("The file %s a symbolic link\n", (S_ISLNK(fileStat.st_mode)) ? "is" : "is not");
 
    //return 0;

    //close(wavfile);
            
    ESP_LOGI(SPIFFS_TAG, "File file has been read");
*/

    //******************************************* SETUP PULSE COUNT CONFIGURATION FOR METAL DETECTOR ***************************************

    ledc_init();

    metaldetector_pcnt_init();
    

    //******************************************* SETUP GPIO CONFIGURATION FOR METAL DETECTOR AND SWITCH ***************************************


    /*//xTaskCreate(example_i2s_adc_dac, "example_i2s_adc_dac", 1024 * 2, NULL, 5, NULL);
    //xTaskCreate(adc_read_task, "ADC read task", 2048, NULL, 5, NULL);
    //gpio_pad_select_gpio(TDA1606_GPIO);
    struct timeval lastPress;
    //ESP_LOGI(LOG_TAG, ">> test1_task");
    gettimeofday(&lastPress, NULL);

    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.pin_bit_mask = (1<<GPIO_GLOVE_OUTPUT_SWITCH);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 0;
    io_conf.pull_down_en = 1;
    gpio_config(&io_conf);

    
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.pin_bit_mask = (1<<GPIO_TDA_INPUT); 
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1;
    io_conf.pull_down_en = 0;
    gpio_config(&io_conf);

    //create a queue to handle gpio event from isr
    q1 = xQueueCreate(10, sizeof(uint32_t));

    xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);

    //install gpio isr service
    gpio_install_isr_service(0); // no flags
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_TDA_INPUT, handler, (void*) GPIO_TDA_INPUT);
    //gpio_isr_handler_add(GPIO_NUM_34, gpio_isr_handler, (void*) GPIO_NUM_34);
    //gpio_isr_handler_add(GPIO_GLOVE_OUTPUT_SWITCH, gpio_isr_handler, (void*) GPIO_GLOVE_OUTPUT_SWITCH);
*/
    //init_gpio();
    //play_theme();
    sound(GPIO_OUTPUT,660,1000);
    sound(GPIO_OUTPUT,950,1000);
    //detect_metal();
    xTaskCreatePinnedToCore(detect_metal, "DetectMetal_Task", 3000, NULL, 3, &DetectMetal_Task, 1);
    
}

static bool get_name_from_eir(uint8_t *eir, uint8_t *bdname, uint8_t *bdname_len)
{
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    return false;
}

static void filter_inquiry_scan_result(esp_bt_gap_cb_param_t *param)
{
    char bda_str[18];
    uint32_t cod = 0;
    int32_t rssi = -129; /* invalid value */
    uint8_t *eir = NULL;
    esp_bt_gap_dev_prop_t *p;

    ESP_LOGI(BT_AV_TAG, "Scanned device: %s", bda2str(param->disc_res.bda, bda_str, 18));
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        p = param->disc_res.prop + i;
        switch (p->type) {
        case ESP_BT_GAP_DEV_PROP_COD:
            cod = *(uint32_t *)(p->val);
            ESP_LOGI(BT_AV_TAG, "--Class of Device: 0x%x", cod);
            break;
        case ESP_BT_GAP_DEV_PROP_RSSI:
            rssi = *(int8_t *)(p->val);
            ESP_LOGI(BT_AV_TAG, "--RSSI: %d", rssi);
            break;
        case ESP_BT_GAP_DEV_PROP_EIR:
            eir = (uint8_t *)(p->val);
            break;
        case ESP_BT_GAP_DEV_PROP_BDNAME:
        default:
            break;
        }
    }

    /* search for device with MAJOR service class as "rendering" in COD */
    if (!esp_bt_gap_is_valid_cod(cod) ||
            !(esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING)) {
        return;
    }

    /* search for device named "ESP_SPEAKER" in its extenXded inqury response */
    if (eir) {
        get_name_from_eir(eir, peer_bdname, NULL);
        if (strcmp((char *)peer_bdname, "SoundCore mini") != 0) {     //jvc ha-fx9bt   SoundCore mini
            return;
        }

        ESP_LOGI(BT_AV_TAG, "Found a target device, address %s, name %s", bda_str, peer_bdname);
        m_a2d_state = APP_AV_STATE_DISCOVERED;
        memcpy(peer_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
        ESP_LOGI(BT_AV_TAG, "Cancel device discovery ...");
        esp_bt_gap_cancel_discovery();
    }
}


void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        filter_inquiry_scan_result(param);
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            if (m_a2d_state == APP_AV_STATE_DISCOVERED) {
                m_a2d_state = APP_AV_STATE_CONNECTING;
                ESP_LOGI(BT_AV_TAG, "Device discovery stopped.");
                ESP_LOGI(BT_AV_TAG, "a2dp connecting to peer: %s", peer_bdname);
                esp_a2d_source_connect(peer_bda);
            } else {
                // not discovered, continue to discover
                ESP_LOGI(BT_AV_TAG, "Device discovery failed, continue to discover...");
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(BT_AV_TAG, "Discovery started.");
        }
        break;
    }
    case ESP_BT_GAP_RMT_SRVCS_EVT:
    case ESP_BT_GAP_RMT_SRVC_REC_EVT:
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:{
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_AV_TAG, "authentication success: %s", param->auth_cmpl.device_name);
            esp_log_buffer_hex(BT_AV_TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            ESP_LOGI(BT_AV_TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
        }
    }
    	break;

    default: {
        ESP_LOGI(BT_AV_TAG, "event: %d", event);
        break;
    }
    }
    return;
}

static void bt_av_hdl_stack_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_AV_TAG, "%s evt %d", __func__, event);
    switch (event) {
    case BT_APP_EVT_STACK_UP: {
        /* set up device name */
        char *dev_name = "UPPER_HAND";
        esp_bt_dev_set_device_name(dev_name);

        /* register GAP callback function */
        esp_bt_gap_register_callback(bt_app_gap_cb);

        /* initialize A2DP source */
        esp_a2d_register_callback(&bt_app_a2d_cb);
        esp_a2d_source_register_data_callback(bt_app_a2d_data_cb);
        esp_a2d_source_init();

        /* set discoverable and connectable mode */
        esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);

        /* start device discovery */
        ESP_LOGI(BT_AV_TAG, "Starting device discovery...");
        m_a2d_state = APP_AV_STATE_DISCOVERING;
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        core = xPortGetCoreID();
        ESP_LOGI(BT_AV_TAG, "BT GAP is using Core : %d\n", core);


        /* create and start heart beat timer */
        do {
            int tmr_id = 0;
            tmr = xTimerCreate("connTmr", (10000 / portTICK_RATE_MS),
                               pdTRUE, (void *)tmr_id, a2d_app_heart_beat);
            xTimerStart(tmr, portMAX_DELAY);//portMAX_DELAY
        } while (0);
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
        break;
    }
}

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    bt_app_work_dispatch(bt_app_av_sm_hdlr, event, param, sizeof(esp_a2d_cb_param_t), NULL);
}

static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len)
{
    if (len < 0 || data == NULL) {
        return 0;
    }

    // generate random sequence
    //int val = rand() % (1 << 16);
    //for (int i = 0; i < (len >> 1); i++) {
    //    data[(i << 1)] = val & 0xff;
    //    data[(i << 1) + 1] = (val >> 8) & 0xff;

/*    int l = read(wavfile, data, len);
    if (wavfile < 0) {
        ESP_LOGI(SPIFFS_TAG, "Failed to open file for writing");
    }
    if (l < len) {
        lseek(wavfile, 0, SEEK_SET);
*/
    int count;

    for (count = 0; count < len ; count++){
        data[count * 2]     = sine_int16[sine_phase];
        data[count * 2 + 1] = sine_int16[sine_phase];
        sine_phase++;
        if (sine_phase >= TABLE_SIZE_441HZ){
            sine_phase -= TABLE_SIZE_441HZ;
        }
    }

    return len;
}

static void a2d_app_heart_beat(void *arg)
{
    bt_app_work_dispatch(bt_app_av_sm_hdlr, BT_APP_HEART_BEAT_EVT, NULL, 0, NULL);
}

static void bt_app_av_sm_hdlr(uint16_t event, void *param)
{
    ESP_LOGI(BT_AV_TAG, "%s state %d, evt 0x%x", __func__, m_a2d_state, event);
    switch (m_a2d_state) {
    case APP_AV_STATE_DISCOVERING:
    case APP_AV_STATE_DISCOVERED:
        break;
    case APP_AV_STATE_UNCONNECTED:
        ESP_LOGI(BT_AV_TAG, "%s AV STATE UNCONNECTED %x", __func__, event);
        bt_app_av_state_unconnected(event, param);
        break;
    case APP_AV_STATE_CONNECTING:
        ESP_LOGI(BT_AV_TAG, "%s AV STATE CONNECTING %x", __func__, event);
        bt_app_av_state_connecting(event, param);
        break;
    case APP_AV_STATE_CONNECTED:
        ESP_LOGI(BT_AV_TAG, "%s AV STATE CONNECTED %x", __func__, event);
        bt_app_av_state_connected(event, param);
        break;
    case APP_AV_STATE_DISCONNECTING:
        ESP_LOGI(BT_AV_TAG, "%s AV STATE DISCONNECTING %x", __func__, event);
        bt_app_av_state_disconnecting(event, param);
        break;
    default:
        ESP_LOGE(BT_AV_TAG, "%s invalid state %d", __func__, m_a2d_state);
        break;
    }
}

static void bt_app_av_state_unconnected(uint16_t event, void *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        break;
    case BT_APP_HEART_BEAT_EVT: {
        uint8_t *p = peer_bda;
        ESP_LOGI(BT_AV_TAG, "a2dp connecting to peer: %02x:%02x:%02x:%02x:%02x:%02x",
                 p[0], p[1], p[2], p[3], p[4], p[5]);
        esp_a2d_source_connect(peer_bda);
        m_a2d_state = APP_AV_STATE_CONNECTING;
        m_connecting_intv = 0;
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
        break;
    }
}

static void bt_app_av_state_connecting(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp connected");
            m_a2d_state =  APP_AV_STATE_CONNECTED;
            m_media_state = APP_AV_MEDIA_STATE_IDLE;
            gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
            gpio_set_level(BLINK_GPIO, 1);
            esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_NONE);
        } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            m_a2d_state =  APP_AV_STATE_UNCONNECTED;
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        break;
    case BT_APP_HEART_BEAT_EVT:
        if (++m_connecting_intv >= 2) {
            m_a2d_state = APP_AV_STATE_UNCONNECTED;
            m_connecting_intv = 0;
        }
        break;
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
        break;
    }
}

static void bt_app_av_media_proc(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;
    switch (m_media_state) {
    case APP_AV_MEDIA_STATE_IDLE: {
        if (event == BT_APP_HEART_BEAT_EVT) {
            ESP_LOGI(BT_AV_TAG, "a2dp media ready checking ...");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        } else if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "a2dp media ready, starting ...");
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                m_media_state = APP_AV_MEDIA_STATE_STARTING;
            }
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STARTING: {
        if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "a2dp media start successfully.");
                m_intv_cnt = 0;
                m_media_state = APP_AV_MEDIA_STATE_STARTED;
            } else {
                // not started succesfully, transfer to idle state
                ESP_LOGI(BT_AV_TAG, "a2dp media start failed.");
                m_media_state = APP_AV_MEDIA_STATE_IDLE;
            }
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STARTED: {
        if (event == BT_APP_HEART_BEAT_EVT) {
            if (++m_intv_cnt >= 2) {   //Was 10
                ESP_LOGI(BT_AV_TAG, "a2dp media stopping...");
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
                m_media_state = APP_AV_MEDIA_STATE_STOPPING;
                m_intv_cnt = 0;
            }
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STOPPING: {
        if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_STOP &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "a2dp media stopped successfully, disconnecting...");
                m_media_state = APP_AV_MEDIA_STATE_IDLE;
                esp_a2d_source_disconnect(peer_bda);
                m_a2d_state = APP_AV_STATE_DISCONNECTING;
            } else {
                ESP_LOGI(BT_AV_TAG, "a2dp media stopping...");
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
            }
        }
        break;
    }
    }
}

static void bt_app_av_state_connected(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp disconnected");
            m_a2d_state = APP_AV_STATE_UNCONNECTED;
            //gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
            //gpio_set_level(BLINK_GPIO, 0);
            esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (ESP_A2D_AUDIO_STATE_STARTED == a2d->audio_stat.state) {
            m_pkt_cnt = 0;
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT:
        // not suppposed to occur for A2DP source
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    case BT_APP_HEART_BEAT_EVT: {
        bt_app_av_media_proc(event, param);
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
        break;
    }
}

static void bt_app_av_state_disconnecting(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp disconnected");
            m_a2d_state =  APP_AV_STATE_UNCONNECTED;
            esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    case BT_APP_HEART_BEAT_EVT:
        break;
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
        break;
    }
}
