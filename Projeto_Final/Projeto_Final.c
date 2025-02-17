#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "hardware/pwm.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/init.h"
#include "pico/cyw43_arch.h"
#include "hardware/irq.h"
#include "hardware/timer.h"

// Configuração dos pinos usados
#define BUZZER_PIN 10       // Buzzer para o ritmo da massagem cardíaca
#define BUZZER_MEDICACAO 21  // Buzzer para o alerta de medicação
#define BOTAO_SILENCIAR 6            // Botão para parar o alerta de medicação
#define BOTAO_LIGAR_DESLIGAR 5 // Botão para iniciar ou parar todo o sistema
#define LED_ALERTA 13         // LED para indicar que a medicação é necessária

// Ajuste dos tempos e sons
#define FREQUENCIA_RITMO 500        // Tom do ritmo da massagem (Hertz)
#define FREQUENCIA_MEDICACAO 440   // Tom do alerta de medicação (Hertz)
#define COMPRESSOES_POR_MINUTO 110                  // Número de compressões no peito por minuto
#define TEMPO_ENTRE_MEDICACOES 30000  // Tempo até lembrar da medicação (milissegundos, ou seja, 30 segundos)
#define DURACAO_BIP_RITMO 100     // Quanto tempo o som da massagem toca (milissegundos)

#define WIFI_SSID "Brunno"
#define WIFI_PASS "12345678"
#define THINGSPEAK_HOST "api.thingspeak.com"
#define THINGSPEAK_PORT 80

#define API_KEY "DZINV0FIFPB8TC71"  // Chave de escrita do ThingSpeak

struct tcp_pcb *tcp_client_pcb;
ip_addr_t server_ip;


bool aplicacao = false;
int injecao = 0;

// Callback quando recebe resposta do ThingSpeak
static err_t http_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (p == NULL) {
        tcp_close(tpcb);
        return ERR_OK;
    }
    printf("Resposta do ThingSpeak: %.*s\n", p->len, (char *)p->payload);
    pbuf_free(p);
    return ERR_OK;
}

// Callback quando a conexão TCP é estabelecida
static err_t http_connected_callback(void *arg, struct tcp_pcb *tpcb, err_t err) {
    if (err != ERR_OK) {
        printf("Erro na conexão TCP: %d\n", err);
        return err;
    }

    printf("Conectado ao ThingSpeak!\n");

    char request[256];
    snprintf(request, sizeof(request),
        "GET /update?api_key=%s&field1=%d HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        API_KEY, injecao, THINGSPEAK_HOST);

    tcp_write(tpcb, request, strlen(request), TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
    tcp_recv(tpcb, http_recv_callback);

    return ERR_OK;
}

// Resolver DNS e conectar ao servidor
static void dns_callback(const char *name,ip_addr_t *ipaddr, void *callback_arg) {
    if (ipaddr) {
        printf("Endereço IP do ThingSpeak: %s\n", ipaddr_ntoa(ipaddr));
        tcp_client_pcb = tcp_new();
        tcp_connect(tcp_client_pcb, ipaddr, THINGSPEAK_PORT, http_connected_callback);
    } else {
        printf("Falha na resolução de DNS\n");
    }
}

// Controle do programa
volatile bool precisa_medicar = false; // Indica se o alerta de medicação está ativo
volatile bool sistema_ligado = false;      // Indica se o sistema está funcionando
volatile uint32_t contador_intervalo = 0; // Contador para gerar as interrupções
volatile bool enviar_sinal_thingspeak = false; // Controle do envio de sinal para ThingSpeak


// Função para gerar um som
void fazer_som(int pino, int frequencia, int duracao) {
    // Calcula o ciclo do som
    int periodo = 1000000 / frequencia;
    int metade_periodo = periodo / 2;
    // Calcula quantos ciclos precisa tocar
    uint32_t num_ciclos = (duracao * 1000) / periodo;

    // Liga e desliga o pino várias vezes para fazer o som
    for (uint32_t i = 0; i < num_ciclos; ++i) {
        gpio_put(pino, 1); // Envia energia para o pino
        busy_wait_us(metade_periodo); // Espera um pouco
        gpio_put(pino, 0); // Corta a energia do pino
        busy_wait_us(metade_periodo); // Espera um pouco
    }
}

// Função para avisar sobre a medicação
void alerta_medicacao() {
    printf("Alerta de medicação disparado.\n");
    while (precisa_medicar) {
        // Sinaliza com o LED
        gpio_put(LED_ALERTA, 1); // Acende o LED
        // Emite o som de alerta
        fazer_som(BUZZER_MEDICACAO, FREQUENCIA_MEDICACAO, 500); // Ajusta duração do som para 500ms
        gpio_put(LED_ALERTA, 0); // Apaga o LED
        sleep_ms(500); // Intervalo entre os bipes e apagamento do LED
    }
    gpio_put(LED_ALERTA, 0); // Garante que o LED esteja apagado ao sair do loop
}

// Função para criar o ritmo da massagem cardíaca
void ritmo_massagem(int pino) {
    // Toca um breve som
    fazer_som(pino, FREQUENCIA_RITMO, DURACAO_BIP_RITMO);
}


// Função que responde aos cliques nos botões
void quando_botao_clicado(uint gpio, uint32_t events) {
    // Se clicou no botão de silenciar a medicação
    if (gpio == BOTAO_SILENCIAR) {
        precisa_medicar = false; // Desativa o alerta
        enviar_sinal_thingspeak = !enviar_sinal_thingspeak; // Alterna o estado de envio de sinal
        printf("Alerta de medicação desativado. Envio de sinal para ThingSpeak %s.\n", enviar_sinal_thingspeak ? "ativado" : "desativado");
    }
    // Se clicou no botão de ligar e desligar o sistema
    else if (gpio == BOTAO_LIGAR_DESLIGAR) {
        sistema_ligado = !sistema_ligado; // Inverte o estado (ligado vira desligado, e vice-versa)
        printf("Sistema foi %s.\n", sistema_ligado ? "ligado" : "desligado");

        // Se desligou o sistema, desativa o alerta também
        if (!sistema_ligado) {
            precisa_medicar = false;
            gpio_put(BUZZER_PIN, 0);   // Desliga o som da massagem
            gpio_put(BUZZER_MEDICACAO, 0); // Desliga o som da medicação
            gpio_put(LED_ALERTA, 0);     // Apaga o LED
        }
    }
}

// Função para a interrupção do temporizador
bool timer_irq(struct repeating_timer *t) {
    if (sistema_ligado) {
        // Mantém o ritmo da massagem
        ritmo_massagem(BUZZER_PIN);
        // Verifica se precisa lembrar da medicação
        contador_intervalo++;
        if (contador_intervalo >= (TEMPO_ENTRE_MEDICACOES / (60000 / COMPRESSOES_POR_MINUTO))) {
            precisa_medicar = true;
            printf("ALERTA: Verificar a medicação!\n");
            contador_intervalo = 0; // Reinicia o contador
            multicore_fifo_push_blocking(0); // Notifica o núcleo 1 para iniciar o alerta de medicação
        }
    } else {
        gpio_put(BUZZER_PIN, 0);
    }
    return true;
}

// Função que o segundo núcleo do Raspberry Pi Pico executa
void segundo_nucleo() {
    // Configura o timer
    struct repeating_timer timer;
    add_repeating_timer_ms(60000 / COMPRESSOES_POR_MINUTO, timer_irq, NULL, &timer);

    while (true) {
        if (multicore_fifo_rvalid()) {
            // Inicia o alerta de medicação
            multicore_fifo_pop_blocking();
            alerta_medicacao();
        }
        tight_loop_contents(); // Mantém o segundo núcleo ocupado
    }
    printf("Segundo núcleo finalizou.\n"); // Quase nunca chega aqui
}

// Função principal do programa
int main() {
    stdio_init_all();

    sleep_ms(5000);

    if (cyw43_arch_init()) {
        printf("Falha ao iniciar Wi-Fi\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();
    printf("Conectando ao Wi-Fi...\n");

    // Tenta conectar ao Wi-Fi repetidamente
    int wifi_retries = 0;
    while (cyw43_arch_wifi_connect_blocking(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_MIXED_PSK)) {
        printf("Falha ao conectar ao Wi-Fi, tentando novamente...\n");
        wifi_retries++;
        if (wifi_retries > 5) { // Limita o número de tentativas
            printf("Número máximo de tentativas de conexão Wi-Fi atingido. Abortando.\n");
            return 1;
        }
        busy_wait_ms(2000); // Espera antes de tentar novamente
    }

    printf("Wi-Fi conectado!\n");

    uint64_t start_time = time_us_64();
    // Resolver DNS do ThingSpeak uma vez após conectar ao Wi-Fi
    dns_gethostbyname(THINGSPEAK_HOST, &server_ip, (dns_found_callback)dns_callback, NULL);

    // Configura os pinos
    gpio_init(BUZZER_PIN);
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);
    gpio_put(BUZZER_PIN, 0);

    gpio_init(BUZZER_MEDICACAO);
    gpio_set_dir(BUZZER_MEDICACAO, GPIO_OUT);
    gpio_put(BUZZER_MEDICACAO, 0);

    gpio_init(BOTAO_SILENCIAR);
    gpio_set_dir(BOTAO_SILENCIAR, GPIO_IN);
    gpio_pull_up(BOTAO_SILENCIAR);

    gpio_init(BOTAO_LIGAR_DESLIGAR);
    gpio_set_dir(BOTAO_LIGAR_DESLIGAR, GPIO_IN);
    gpio_pull_up(BOTAO_LIGAR_DESLIGAR);

    gpio_init(LED_ALERTA);
    gpio_set_dir(LED_ALERTA, GPIO_OUT);
    gpio_put(LED_ALERTA, 0);

    // Avisa quando os botões forem clicados
    gpio_set_irq_enabled_with_callback(BOTAO_SILENCIAR, GPIO_IRQ_EDGE_FALL, true, &quando_botao_clicado);
    gpio_set_irq_enabled_with_callback(BOTAO_LIGAR_DESLIGAR, GPIO_IRQ_EDGE_FALL, true, &quando_botao_clicado);

    // Inicia o segundo núcleo
    multicore_launch_core1(segundo_nucleo);

    printf("Sistema pronto. Aperte o botão para começar.\n");

    // Loop principal do programa
    while (true) {
        tight_loop_contents(); // Mantém o núcleo principal ocupado
        if (time_us_64() - start_time > 90000000) {
            dns_gethostbyname(THINGSPEAK_HOST, &server_ip, (dns_found_callback)dns_callback, NULL); // Resolve o endereço IP do servidor
            start_time = time_us_64();
        }

        if (!gpio_get(BOTAO_SILENCIAR)){
            if(!gpio_get(BOTAO_SILENCIAR) != aplicacao){
                injecao++;
                // beep(21, 100);
            aplicacao = true;
            sleep_ms(10);
            }

        } else {
            aplicacao = false;    
        }
    }

    return 0;
}
