// V2 pins and codec slot mapping follow Waveshare upstream 8ead4a9.
#include "terminal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "vendor/esp_lcd_st77916.h"
#include "vendor/panel_variant.h"

i2c_master_bus_handle_t xy_i2c;
i2s_chan_handle_t xy_tx, xy_rx;
esp_lcd_panel_handle_t xy_panel;
static SemaphoreHandle_t display_te;
static spi_device_handle_t lcd_spi;
static uint16_t *lcd_dma[2];
volatile uint32_t xy_te_count,xy_te_timeouts;
static esp_codec_dev_handle_t adc, dac;

static void IRAM_ATTR te_edge(void *arg){
    BaseType_t woken=pdFALSE;xy_te_count++;xSemaphoreGiveFromISR(display_te,&woken);
    if(woken)portYIELD_FROM_ISR();
}
bool display_wait_te(void){
    xSemaphoreTake(display_te,0);
    if(xSemaphoreTake(display_te,pdMS_TO_TICKS(50))==pdTRUE)return true;
    xy_te_timeouts++;return false;
}
static void lcd_param(uint8_t command,const uint8_t *data,size_t bytes){
    spi_transaction_t t={.cmd=0x02,.addr=(uint32_t)command<<8,.length=bytes*8,.tx_buffer=data};
    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd_spi,&t));
}
void display_present(const uint16_t *frame){
    // Give the short internal-buffer copy/refill priority over compute tasks;
    // while waiting for DMA, microphone tasks can still run normally.
    UBaseType_t priority=uxTaskPriorityGet(NULL);vTaskPrioritySet(NULL,8);
    ESP_ERROR_CHECK(spi_device_acquire_bus(lcd_spi,portMAX_DELAY));
    lcd_param(0x2a,(uint8_t[]){0,0,1,0x67},4);
    lcd_param(0x2b,(uint8_t[]){0,0,1,0x67},4);
    spi_transaction_ext_t transactions[2]={0};spi_transaction_t *completed;
    unsigned pending=0;
    for(unsigned stripe=0;stripe<9;stripe++){
        if(pending==2){ESP_ERROR_CHECK(spi_device_get_trans_result(lcd_spi,&completed,portMAX_DELAY));pending--;}
        unsigned slot=stripe%2;memcpy(lcd_dma[slot],frame+stripe*360*40,360*40*2);
        spi_transaction_ext_t *t=&transactions[slot];memset(t,0,sizeof(*t));
        t->base.cmd=0x32;t->base.addr=0x002c00;
        t->command_bits=stripe==0?8:0;t->address_bits=stripe==0?24:0;
        t->base.flags=SPI_TRANS_MODE_QIO|SPI_TRANS_VARIABLE_CMD|SPI_TRANS_VARIABLE_ADDR;
        if(stripe<8)t->base.flags|=SPI_TRANS_CS_KEEP_ACTIVE;
        t->base.length=360*40*16;t->base.tx_buffer=lcd_dma[slot];
        ESP_ERROR_CHECK(spi_device_queue_trans(lcd_spi,&t->base,portMAX_DELAY));pending++;
    }
    while(pending--){ESP_ERROR_CHECK(spi_device_get_trans_result(lcd_spi,&completed,portMAX_DELAY));}
    spi_device_release_bus(lcd_spi);vTaskPrioritySet(NULL,priority);
}

static void panel_init(void) {
    i2c_master_dev_handle_t exio;
    i2c_device_config_t ec={.device_address=0x20,.scl_speed_hz=400000};
    ESP_ERROR_CHECK(i2c_master_bus_add_device(xy_i2c,&ec,&exio));
    ESP_ERROR_CHECK(i2c_master_transmit(exio,(uint8_t[]){3,0},2,500));
    ESP_ERROR_CHECK(i2c_master_transmit(exio,(uint8_t[]){1,0x01},2,500));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK(i2c_master_transmit(exio,(uint8_t[]){1,0x03},2,500));
    vTaskDelay(pdMS_TO_TICKS(100));
    spi_bus_config_t bus={.sclk_io_num=40,.data0_io_num=46,.data1_io_num=45,.data2_io_num=42,.data3_io_num=41,.max_transfer_sz=360*40*2};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io={.cs_gpio_num=21,.dc_gpio_num=-1,.spi_mode=0,.pclk_hz=3000000,.trans_queue_depth=2,.lcd_cmd_bits=32,.lcd_param_bits=8,.flags.quad_mode=1};
    esp_lcd_panel_io_handle_t ih;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,&io,&ih));
    uint8_t id[4]={0};
    esp_err_t iderr=esp_lcd_panel_io_rx_param(ih,0x03000400,id,4);
    ESP_LOGI("panel","ID %02x %02x %02x %02x (%s)",id[0],id[1],id[2],id[3],esp_err_to_name(iderr));
    ESP_ERROR_CHECK(esp_lcd_panel_io_del(ih));
    io.pclk_hz=80000000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,&io,&ih));
    st77916_vendor_config_t vendor={.flags.use_qspi_interface=1};
    if(iderr==ESP_OK && id[1]==2 && id[2]==0x7f) {
        vendor.init_cmds=vendor_specific_init_new;
        vendor.init_cmds_size=sizeof(vendor_specific_init_new)/sizeof(vendor_specific_init_new[0]);
    }
    esp_lcd_panel_dev_config_t pc={.reset_gpio_num=-1,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.bits_per_pixel=16,.vendor_config=&vendor};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(ih,&pc,&xy_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(xy_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(xy_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(xy_panel,true));
    // V2 LCD_TE is GPIO18 (Waveshare schematic and board header).
    display_te=xSemaphoreCreateBinary();
    gpio_config_t te={.pin_bit_mask=1ULL<<18,.mode=GPIO_MODE_INPUT,.intr_type=GPIO_INTR_POSEDGE};
    ESP_ERROR_CHECK(gpio_config(&te));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(18,te_edge,NULL));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(ih,0x02003500,(uint8_t[]){0},1));
    // Initialization used the vendor panel driver. Transfer ownership of CS21
    // to one SPI device that can keep CS asserted across the entire frame.
    ESP_ERROR_CHECK(esp_lcd_panel_del(xy_panel));xy_panel=NULL;
    ESP_ERROR_CHECK(esp_lcd_panel_io_del(ih));
    spi_device_interface_config_t stream={.clock_speed_hz=80000000,.mode=0,.spics_io_num=21,.queue_size=2,
        .command_bits=8,.address_bits=24,.flags=SPI_DEVICE_HALFDUPLEX};
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST,&stream,&lcd_spi));
    for(int i=0;i<2;i++){lcd_dma[i]=heap_caps_malloc(360*40*2,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA);assert(lcd_dma[i]);}
    ledc_timer_config_t timer={.speed_mode=LEDC_LOW_SPEED_MODE,.duty_resolution=LEDC_TIMER_10_BIT,.timer_num=LEDC_TIMER_0,.freq_hz=5000,.clk_cfg=LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t light={.gpio_num=5,.speed_mode=LEDC_LOW_SPEED_MODE,.channel=LEDC_CHANNEL_0,.timer_sel=LEDC_TIMER_0,.duty=450};
    ESP_ERROR_CHECK(ledc_channel_config(&light));
}

void board_init(void) {
    i2c_master_bus_config_t bc={.i2c_port=0,.sda_io_num=11,.scl_io_num=10,.clk_source=I2C_CLK_SRC_DEFAULT,.flags.enable_internal_pullup=true};
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc,&xy_i2c));
    ESP_ERROR_CHECK(i2c_master_probe(xy_i2c,0x18,500));
    ESP_ERROR_CHECK(i2c_master_probe(xy_i2c,0x40,500));
    panel_init();
    i2s_chan_config_t ch=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,I2S_ROLE_MASTER);
    ch.dma_desc_num=6; ch.dma_frame_num=160;
    ch.auto_clear_after_cb=false;
    ESP_ERROR_CHECK(i2s_new_channel(&ch,&xy_tx,&xy_rx));
    i2s_std_config_t std={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(16000),.slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,I2S_SLOT_MODE_STEREO),.gpio_cfg={.mclk=2,.bclk=48,.ws=38,.dout=47,.din=39}};
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(xy_tx,&std));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(xy_rx,&std));
    audio_codec_i2s_cfg_t data={.port=I2S_NUM_1,.tx_handle=xy_tx,.rx_handle=xy_rx};
    const audio_codec_data_if_t *dif=audio_codec_new_i2s_data(&data);
    audio_codec_i2c_cfg_t ac={.addr=ES7210_CODEC_DEFAULT_ADDR,.bus_handle=xy_i2c};
    es7210_codec_cfg_t adc_cfg={.ctrl_if=audio_codec_new_i2c_ctrl(&ac),.mic_selected=ES7210_SEL_MIC1|ES7210_SEL_MIC2|ES7210_SEL_MIC3|ES7210_SEL_MIC4};
    esp_codec_dev_cfg_t dev={.codec_if=es7210_codec_new(&adc_cfg),.data_if=dif,.dev_type=ESP_CODEC_DEV_TYPE_IN};
    adc=esp_codec_dev_new(&dev);
    ac.addr=ES8311_CODEC_DEFAULT_ADDR;
    es8311_codec_cfg_t dac_cfg={.ctrl_if=audio_codec_new_i2c_ctrl(&ac),.gpio_if=audio_codec_new_gpio(),.codec_mode=ESP_CODEC_DEV_WORK_MODE_DAC,.pa_pin=-1,.use_mclk=false};
    dev.codec_if=es8311_codec_new(&dac_cfg);dev.dev_type=ESP_CODEC_DEV_TYPE_OUT;
    dac=esp_codec_dev_new(&dev);
    esp_codec_dev_sample_info_t fmt={.sample_rate=16000,.channel=2,.bits_per_sample=32};
    ESP_ERROR_CHECK(esp_codec_dev_open(adc,&fmt));
    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(adc,30));
    ESP_ERROR_CHECK(esp_codec_dev_open(dac,&fmt));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dac,55));
    // The codec data interface enabled both channels. Register ISR while stopped.
    i2s_channel_disable(xy_tx);
    playback_init();
    ESP_ERROR_CHECK(i2s_channel_enable(xy_tx));
    gpio_set_direction(15,GPIO_MODE_OUTPUT);gpio_set_level(15,1);
    gpio_set_direction(0,GPIO_MODE_INPUT);gpio_pullup_en(0);
    ESP_LOGI("board","V2 codecs and panel initialized");
}
