#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"
#include <stdlib.h>

#include <string.h>
#include <time.h>
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "inc/ssd1306.h"

// I2C defines
// This example will use I2C0 on GPIO8 (SDA) and GPIO9 (SCL) running at 400KHz.
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define I2C_PORT i2c0
#define I2C_SDA 8
#define I2C_SCL 9

// config wifi
#define WIFI_SSID "wifimo"
#define WIFI_PASSWORD "abcd@1234"

// Configuração do display OLED 
#define I2C_SDA_PIN_OLED 14 
#define I2C_SCL_PIN_OLED 15
#define OLED_WIDTH 128 
#define OLED_HEIGHT 32 

// Configurações do DS3231
#define DS3231_I2C_ADDRESS 0x68
#define DS3231_REG_SECONDS 0x00
#define DS3231_REG_MINUTES 0x01
#define DS3231_REG_HOURS   0x02
#define DS3231_REG_DAY     0x03
#define DS3231_REG_DATE    0x04
#define DS3231_REG_MONTH   0x05
#define DS3231_REG_YEAR    0x06
#define DS3231_REG_CONTROL 0x0E

// Configurações I2C DS3231
#define I2C_PORT i2c0
#define I2C_SDA_PIN 0
#define I2C_SCL_PIN 1
#define I2C_BAUDRATE 100000

// Configurações NTP
#define NTP_MSG_LEN 48
#define NTP_PORT 123
#define NTP_DELTA 2208988800 // Diferença entre 1900 e 1970
#define NTP_TIMEOUT (30 * 1000)
#define NTP_SERVER "pool.ntp.org"
#define NTP_DELTA 2208988800 // seconds between 1 Jan 1900 and 1 Jan 1970
#define NTP_TEST_TIME_MS (60 * 1000) // uma requisição por minuto para o servidor NTP
#define NTP_RESEND_TIME_MS (10 * 1000)

// Configuração de Timezone
#define TIMEZONE_OFFSET_HOURS (-3)  // UTC-3 (Brasília)
#define TIMEZONE_OFFSET_SECONDS (TIMEZONE_OFFSET_HOURS * 3600)

// Estado da sincronização NTP
typedef struct {
    ip_addr_t ntp_server_address;
    bool dns_request_sent;
    struct udp_pcb *ntp_pcb;
    absolute_time_t ntp_test_time;
    alarm_id_t ntp_resend_alarm;
    bool time_synced;
} ntp_state_t;

// Estrutura para armazenar data/hora no modulo DS3231
typedef struct {
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day;
    uint8_t date;
    uint8_t month;
    uint8_t year;
} rtc_datetime_t;

static ntp_state_t ntp_state = {0};

void oled_setup() { 
    i2c_init(i2c1, ssd1306_i2c_clock * 1000);
    gpio_set_function(I2C_SDA_PIN_OLED, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN_OLED, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN_OLED);
    gpio_pull_up(I2C_SCL_PIN_OLED);
    ssd1306_init();     
}

void show_message_oled(char* message[], int lines) {
    struct render_area frame_area = {
        start_column : 0,
        end_column : ssd1306_width - 1,
        start_page : 0,
        end_page : ssd1306_n_pages - 1
    };

    calculate_render_area_buffer_length(&frame_area);
    // limpa o display
    uint8_t ssd[ssd1306_buffer_length];
    memset(ssd, 0, ssd1306_buffer_length);
    render_on_display(ssd, &frame_area);

    int y = 0;

    for (uint i = 0; i < lines; i++) {
        // printf("imprimindo na tela: %s\n", message[i]);
        ssd1306_draw_string(ssd,5,y,message[i]);
        // movendo para a proxima "linha" no display
        y += 8;
    }
    render_on_display(ssd, &frame_area);
}

// Função para converter BCD para decimal
uint8_t bcd_to_decimal(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

// Função para converter decimal para BCD
uint8_t decimal_to_bcd(uint8_t decimal) {
    return ((decimal / 10) << 4) | (decimal % 10);
}

// Inicializar I2C
void init_i2c() {
    i2c_init(I2C_PORT, I2C_BAUDRATE);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    
    printf("I2C inicializado - SDA: GPIO%d, SCL: GPIO%d\n", I2C_SDA_PIN, I2C_SCL_PIN);
}

// Verificar se DS3231 está conectado
bool ds3231_is_connected() {
    uint8_t data;
    int result = i2c_read_blocking(I2C_PORT, DS3231_I2C_ADDRESS, &data, 1, false);
    return result == 1;
}

// Ler registrador do DS3231
uint8_t ds3231_read_register(uint8_t reg) {
    uint8_t data;
    int w_err = i2c_write_blocking(I2C_PORT, DS3231_I2C_ADDRESS, &reg, 1, true);
    // if (w_err != 0) {
    //     printf("\nErro em escrita i2c no ds3231: %d\n", w_err);
    // }
    int r_err = i2c_read_blocking(I2C_PORT, DS3231_I2C_ADDRESS, &data, 1, false);
    // if (r_err != 0) {
    //     printf("\nErro em leitura i2c no ds3231: %d\n", r_err);
    // }
    return data;
}

// Escrever registrador do DS3231
void ds3231_write_register(uint8_t reg, uint8_t data) {
    uint8_t buffer[2] = {reg, data};
    i2c_write_blocking(I2C_PORT, DS3231_I2C_ADDRESS, buffer, 2, false);
}

// Inicializar DS3231
bool ds3231_init() {
    if (!ds3231_is_connected()) {
        printf("ERRO: DS3231 não encontrado no endereço 0x%02X\n", DS3231_I2C_ADDRESS);
        return false;
    }
    
    printf("DS3231 conectado com sucesso!\n");    
    // Configurar registrador de controle (desabilitar alarmes, habilitar oscilador)
    ds3231_write_register(DS3231_REG_CONTROL, 0x00);
    
    return true;
}

// Configurar data/hora no DS3231 (apenas se necessário)
void ds3231_set_datetime(rtc_datetime_t *datetime) {
    ds3231_write_register(DS3231_REG_SECONDS, decimal_to_bcd(datetime->seconds));
    ds3231_write_register(DS3231_REG_MINUTES, decimal_to_bcd(datetime->minutes));
    ds3231_write_register(DS3231_REG_HOURS, decimal_to_bcd(datetime->hours));
    ds3231_write_register(DS3231_REG_DAY, decimal_to_bcd(datetime->day));
    ds3231_write_register(DS3231_REG_DATE, decimal_to_bcd(datetime->date));
    ds3231_write_register(DS3231_REG_MONTH, decimal_to_bcd(datetime->month));
    ds3231_write_register(DS3231_REG_YEAR, decimal_to_bcd(datetime->year));
    
    printf("Data/hora configurada: %02d/%02d/20%02d %02d:%02d:%02d\n",
           datetime->date, datetime->month, datetime->year,
           datetime->hours, datetime->minutes, datetime->seconds);
}

// Ler data/hora do DS3231
void ds3231_get_datetime(rtc_datetime_t *datetime) {
    datetime->seconds = bcd_to_decimal(ds3231_read_register(DS3231_REG_SECONDS) & 0x7F);
    datetime->minutes = bcd_to_decimal(ds3231_read_register(DS3231_REG_MINUTES));
    datetime->hours = bcd_to_decimal(ds3231_read_register(DS3231_REG_HOURS) & 0x3F);
    datetime->day = bcd_to_decimal(ds3231_read_register(DS3231_REG_DAY));
    datetime->date = bcd_to_decimal(ds3231_read_register(DS3231_REG_DATE));
    datetime->month = bcd_to_decimal(ds3231_read_register(DS3231_REG_MONTH) & 0x7F);
    datetime->year = bcd_to_decimal(ds3231_read_register(DS3231_REG_YEAR));
}

// Gerar timestamp formatado
void generate_timestamp(char *buffer, size_t buffer_size) {
    rtc_datetime_t current_time;
    ds3231_get_datetime(&current_time);

    char s_data[20];
    char s_tempo[20];

    // sprintf(s_data, "%d/%d/%d", current_time.date, current_time.month, current_time.year);
    snprintf(s_data, sizeof(s_data), "data %02d %02d %02d", current_time.date,
              current_time.month, current_time.year);
    snprintf(s_tempo, sizeof(s_tempo), "hora %02d %02d %02d", current_time.hours,
              current_time.minutes, current_time.seconds);
    char *showing_text[] = {
        s_data,
        s_tempo
    };

    show_message_oled(showing_text,2);
    
    // Formato: YYYY-MM-DD HH:MM:SS
    snprintf(buffer, buffer_size, "20%02d-%02d-%02d %02d:%02d:%02d",
             current_time.year, current_time.month, current_time.date,
             current_time.hours, current_time.minutes, current_time.seconds);
}

// Função para simular leitura de sensor com timestamp
void sensor_reading_with_timestamp() {

    

    char timestamp[32];
    generate_timestamp(timestamp, sizeof(timestamp));

    
    // Simular leitura de sensor (substitua pela sua leitura real)
    float sensor_value = 25.6; // Exemplo: temperatura em Celsius    
    printf("[%s] Sensor: %.2f°C\n", timestamp, sensor_value);
}


typedef struct NTP_T_ {
    ip_addr_t ntp_server_address;
    struct udp_pcb *ntp_pcb;
    async_at_time_worker_t request_worker;
    async_at_time_worker_t resend_worker;
} NTP_T;



// Called with results of operation
static void ntp_result(NTP_T* state, int status, time_t *result) {
    if (status == 0 && result) {
        // struct tm *utc = gmtime(result);

        time_t local_time = *result + TIMEZONE_OFFSET_SECONDS;
        struct tm *local_tm = gmtime(&local_time);
        
        printf("NTP sincronizado: %02d/%02d/%04d %02d:%02d:%02d (UTC%+d)\n", 
               local_tm->tm_mday, local_tm->tm_mon + 1, local_tm->tm_year + 1900,
               local_tm->tm_hour, local_tm->tm_min, local_tm->tm_sec, TIMEZONE_OFFSET_HOURS);


        rtc_datetime_t initial_time = {
            .seconds = local_tm->tm_sec,
            .minutes = local_tm->tm_min,
            .hours = local_tm->tm_hour,
            .day = local_tm->tm_wday,      // 1=domingo, 2=segunda, etc.
            .date = local_tm->tm_mday,     // dia do mês
            .month = local_tm->tm_mon + 1,    // setembro
            .year = local_tm->tm_year % 100     // 2025 (apenas os últimos 2 dígitos)
        };
        ds3231_set_datetime(&initial_time);
    }
    async_context_remove_at_time_worker(cyw43_arch_async_context(), &state->resend_worker);
    hard_assert(async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(),  &state->request_worker, NTP_TEST_TIME_MS)); // repeat the request in future
    // printf("Next request in %ds\n", NTP_TEST_TIME_MS / 1000);
}

// Make an NTP request
static void ntp_request(NTP_T *state) {
    // cyw43_arch_lwip_begin/end should be used around calls into lwIP to ensure correct locking.
    // You can omit them if you are in a callback from lwIP. Note that when using pico_cyw_arch_poll
    // these calls are a no-op and can be omitted, but it is a good practice to use them in
    // case you switch the cyw43_arch type later.
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
    uint8_t *req = (uint8_t *) p->payload;
    memset(req, 0, NTP_MSG_LEN);
    req[0] = 0x1b;
    udp_sendto(state->ntp_pcb, p, &state->ntp_server_address, NTP_PORT);
    pbuf_free(p);
    cyw43_arch_lwip_end();
}

// Call back with a DNS result
static void ntp_dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
    NTP_T *state = (NTP_T*)arg;
    if (ipaddr) {
        state->ntp_server_address = *ipaddr;
        printf("ntp address %s\n", ipaddr_ntoa(ipaddr));
        ntp_request(state);
    } else {
        printf("ntp dns request failed\n");
        ntp_result(state, -1, NULL);
    }
}

// NTP data received
static void ntp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    NTP_T *state = (NTP_T*)arg;
    uint8_t mode = pbuf_get_at(p, 0) & 0x7;
    uint8_t stratum = pbuf_get_at(p, 1);

    // Check the result
    if (ip_addr_cmp(addr, &state->ntp_server_address) && port == NTP_PORT && p->tot_len == NTP_MSG_LEN &&
        mode == 0x4 && stratum != 0) {
        uint8_t seconds_buf[4] = {0};
        pbuf_copy_partial(p, seconds_buf, sizeof(seconds_buf), 40);
        uint32_t seconds_since_1900 = seconds_buf[0] << 24 | seconds_buf[1] << 16 | seconds_buf[2] << 8 | seconds_buf[3];
        uint32_t seconds_since_1970 = seconds_since_1900 - NTP_DELTA;
        time_t epoch = seconds_since_1970;
        ntp_result(state, 0, &epoch);
    } else {
        printf("invalid ntp response\n");
        ntp_result(state, -1, NULL);
    }
    pbuf_free(p);
}

// Called to make a NTP request
static void request_worker_fn(__unused async_context_t *context, async_at_time_worker_t *worker) {
    NTP_T* state = (NTP_T*)worker->user_data;
    hard_assert(async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &state->resend_worker, NTP_RESEND_TIME_MS)); // in case UDP request is lost
    int err = dns_gethostbyname(NTP_SERVER, &state->ntp_server_address, ntp_dns_found, state);
    if (err == ERR_OK) {
        ntp_request(state); // Cached DNS result, make NTP request
    } else if (err != ERR_INPROGRESS) { // ERR_INPROGRESS means expect a callback
        printf("dns request failed\n");
        ntp_result(state, -1, NULL);
    }
}

// Called to resend an NTP request if it appears to get lost
static void resend_worker_fn(__unused async_context_t *context, async_at_time_worker_t *worker) {
    NTP_T* state = (NTP_T*)worker->user_data;
    printf("ntp request failed\n");
    ntp_result(state, -1, NULL);
}

// Perform initialisation
static NTP_T* ntp_init(void) {
    NTP_T *state = (NTP_T*)calloc(1, sizeof(NTP_T));
    if (!state) {
        printf("failed to allocate state\n");
        return NULL;
    }
    state->ntp_pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (!state->ntp_pcb) {
        printf("failed to create pcb\n");
        free(state);
        return NULL;
    }
    udp_recv(state->ntp_pcb, ntp_recv, state);
    state->request_worker.do_work = request_worker_fn;
    state->request_worker.user_data = state;
    state->resend_worker.do_work = resend_worker_fn;
    state->resend_worker.user_data = state;
    return state;
}

// Runs ntp test forever
void init_ntp_client(void) {
    NTP_T *state = ntp_init();
    if (!state)
        return;
    hard_assert(async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(),  &state->request_worker, 0)); // make the first request

    // free(state);
}

int main()
{
    stdio_init_all();
    oled_setup();
    sleep_ms(1000);
    printf("Aguardando sincronização NTP...\n");    

    if (cyw43_arch_init()) {
        printf("failed to initialise\n");
        return 1;
    }
    cyw43_arch_enable_sta_mode();

    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("failed to connect\n");
        return 1;
    }
    // inicializar chamadas ao ntp de forma assincrona
    init_ntp_client();
    
    // Inicializar I2C
    init_i2c();
    
    // Aguardar um pouco para estabilizar
    sleep_ms(1000);
    
    // Inicializar DS3231
    if (!ds3231_init()) {
        printf("Falha ao inicializar DS3231. Verifique as conexões.\n");
        return -1;
    }
        
    printf("\nSistema inicializado! Gerando timestamps...\n");
    printf("Pressione Ctrl+C para parar\n\n");
    
    // Loop principal - gerar timestamps a cada 5 segundos
    while (true) {
        sensor_reading_with_timestamp();
        sleep_ms(5000); // Aguardar 5 segundos
    }
    
    return 0;   
}
