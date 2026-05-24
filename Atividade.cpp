/*
 * Projeto: Publicacao de temperatura e controle de LED via MQTT
 * Hardware: Raspberry Pi Pico W (RP2040 + chip Wi-Fi CYW43439)
 *
 * Fluxo geral:
 *   1. Conecta ao Wi-Fi.
 *   2. Conecta ao broker MQTT.
 *   3. Se inscreve no topico de LED para receber comandos externos.
 *   4. No loop principal:
 *      - Botao pressionado → le a temperatura da CPU e publica via MQTT.
 *      - Mensagem recebida no topico LED → ajusta o intervalo de piscar.
 */

/* Bibliotecas padrao C */
#include <stdio.h>   // printf, snprintf
#include <stdlib.h>  // strtol (converte string para numero)
#include <string.h>  // memcpy, strlen
#include <ctype.h>   // isspace (detecta espacos/tabs/newlines)

/* SDK do Raspberry Pi Pico */
#include "pico/stdlib.h"      // GPIO, timers, sleep_ms
#include "pico/cyw43_arch.h"  // driver do chip Wi-Fi CYW43439 (controla Wi-Fi e LED integrado)
#include "lwip/apps/mqtt.h"   // cliente MQTT sobre TCP/IP (lwIP = Lightweight IP stack)
#include "bsp/board.h"        // board_button_read() — le o botao integrado da placa
#include "hardware/adc.h"     // ADC — conversor analogico-digital para ler o sensor de temperatura

/* Endereco IP do broker MQTT (servidor que recebe e distribui as mensagens) */
#define END_MQTT "34.243.217.54"

/* Topicos MQTT — identifica onde as mensagens serao publicadas/recebidas.
 * Formato: /nome/ra/funcao  — ajuste para seu nome e RA antes da entrega. */
#define TOPICO_TEMPERATURA "/pedro_coelho/240025703/temperatura"
#define TOPICO_LED         "/pedro_coelho/240025703/led"

/* Tempo minimo entre leituras do botao para evitar multiplos disparos
 * por um unico pressionamento (debounce). */
#define DEBOUNCE_MS 250u

/* Informacoes de identificacao do cliente MQTT enviadas ao broker na conexao.
 * client_id vazio → o broker gera um ID automatico.
 * Keep-alive de 60 s → o cliente envia um "ping" ao broker a cada 60 s para manter a conexao viva. */
struct mqtt_connect_client_info_t info_cliente = {
    "",    // client_id
    NULL,  // usuario (sem autenticacao)
    NULL,  // senha
    60,    // keep-alive em segundos
    NULL,  // will_topic (mensagem de "ultimo recurso" — nao usado)
    NULL,  // will_msg
    0,     // will_qos
    0      // will_retain
};

/* Flag que indica se a conexao MQTT foi aceita pelo broker.
 * 'volatile' porque e alterada dentro de callbacks (interrupcoes de rede). */
static volatile bool conectado_com_sucesso = false;

/* Intervalo atual de piscar do LED em segundos.
 * 0 = LED desligado. Alterada pelo callback de dados MQTT. */
static volatile int32_t intervalo_led_segundos = 0;

/*
 * Callback chamado apos publicar uma mensagem ou assinar um topico.
 * O lwIP chama esta funcao para confirmar se a operacao foi bem-sucedida.
 * 'error' == ERR_OK (0) significa sucesso.
 */
static void mqtt_requisicao_cb(void *arg, err_t error) {
    (void)arg;
    printf("Status da requisicao MQTT: %d\n", error);
}

/*
 * Callback chamado quando o broker avisa que uma nova mensagem chegou.
 * Neste momento so temos o nome do topico; o conteudo vem logo apos
 * em mqtt_dados_recebidos_cb (o lwIP pode dividir payloads grandes em
 * varios fragmentos).
 */
static void mqtt_recebendo_publicacao_cb(void *arg, const char *topico, uint32_t tamanho) {
    (void)arg;
    (void)tamanho;
    printf("Mensagem recebida no topico: %s\n", topico);
}

/*
 * Callback chamado com o conteudo (payload) da mensagem recebida.
 * Responsabilidade: interpretar o valor e ajustar o intervalo do LED.
 *
 * Regras:
 *   - Inteiro positivo → LED pisca nesse intervalo (em segundos).
 *   - 0 ou texto invalido → LED apaga e timer para.
 */
static void mqtt_dados_recebidos_cb(void *arg, const uint8_t *dados, uint16_t tamanho, uint8_t flags) {
    (void)arg;
    (void)flags;

    char payload[64];

    /* Protecao contra payloads maiores que o buffer — rejeita e desliga o LED. */
    if (tamanho >= sizeof(payload)) {
        intervalo_led_segundos = 0;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        printf("Payload invalido: tamanho excedido\n");
        return;
    }

    /* Copia os bytes recebidos e adiciona terminador de string. */
    memcpy(payload, dados, tamanho);
    payload[tamanho] = '\0';

    /* Remove espacos em branco no inicio e no fim da string.
     * Isso permite aceitar payloads como "5\n" ou " 3 " sem erros. */
    char *inicio = payload;
    while (*inicio && isspace((unsigned char)*inicio)) {
        inicio++;
    }

    char *fim = inicio + strlen(inicio);
    while (fim > inicio && isspace((unsigned char)*(fim - 1))) {
        fim--;
    }
    *fim = '\0';

    /* strtol converte a string para long (base 10).
     * 'end_ptr' aponta para o primeiro caractere que NAO e parte do numero.
     * Se end_ptr aponta para '\0', toda a string era um numero valido. */
    char *end_ptr = NULL;
    long valor = strtol(inicio, &end_ptr, 10);
    bool inteiro_valido = (inicio[0] != '\0') && (end_ptr != NULL) && (*end_ptr == '\0');

    if (inteiro_valido && valor > 0 && valor <= INT32_MAX) {
        /* Valor valido, positivo e dentro do limite de int32 — atualiza o intervalo de piscar.
         * O limite INT32_MAX evita overflow silencioso no cast para int32_t. */
        intervalo_led_segundos = (int32_t)valor;
        printf("Novo intervalo do LED: %ld s\n", valor);
        return;
    }

    /* Valor zero, negativo ou texto invalido: para o LED imediatamente. */
    intervalo_led_segundos = 0;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
    printf("LED desligado; payload recebido: %s\n", inicio);
}

/*
 * Callback chamado quando a tentativa de conexao ao broker MQTT e concluida.
 * Se aceita, assina o topico de controle do LED para comecar a receber comandos.
 * QoS 0 = "fire and forget" (sem confirmacao de entrega).
 */
static void mqtt_conectado_cb(mqtt_client_t *cliente, void *arg, mqtt_connection_status_t status) {
    (void)arg;
    printf("Cliente MQTT conectado com status: %d\n", status);

    if (status != MQTT_CONNECT_ACCEPTED) {
        /* Broker recusou ou houve erro de rede — marca como desconectado. */
        conectado_com_sucesso = false;
        return;
    }

    conectado_com_sucesso = true;
    printf("Conexao MQTT aceita\n");

    /* Assina o topico do LED assim que a conexao e estabelecida. */
    err_t sub_status = mqtt_subscribe(cliente, TOPICO_LED, 0, mqtt_requisicao_cb, NULL);
    if (sub_status == ERR_OK) {
        printf("Inscrito em %s\n", TOPICO_LED);
    } else {
        printf("Falha ao assinar %s (erro %d)\n", TOPICO_LED, sub_status);
    }
}

/*
 * Le a temperatura interna da CPU usando o sensor ADC do RP2040.
 *
 * O sensor esta no canal ADC 4. A formula oficial do datasheet converte
 * a tensao lida para graus Celsius:
 *   T = 27 - (V - 0.706) / 0.001721
 * onde V e a tensao correspondente ao valor digital lido.
 */
static float ler_temperatura_cpu_celsius(void) {
    const float fator_conversao = 3.3f / (1 << 12); // 3.3 V / 4096 niveis (ADC de 12 bits)
    uint16_t leitura_adc = adc_read();               // Valor bruto de 0 a 4095
    float tensao = leitura_adc * fator_conversao;    // Converte para volts
    return 27.0f - (tensao - 0.706f) / 0.001721f;   // Converte volts para Celsius
}

int main() {
    /* Inicializa USB/UART para que os printf apareçam no terminal serial. */
    stdio_init_all();

    /* Aguarda 3 s para dar tempo de abrir o monitor serial antes dos primeiros logs. */
    sleep_ms(3000);

    /* Inicializa o chip Wi-Fi CYW43439 (controla também o LED integrado na Pico W). */
    if (cyw43_arch_init()) {
        printf("Falha ao inicializar Wi-Fi\n");
        return -1;
    }

    /* Coloca o Wi-Fi em modo estacao (STA) — conecta a um roteador existente.
     * O modo alternativo seria AP (Access Point), onde a Pico criaria sua propria rede. */
    cyw43_arch_enable_sta_mode();
    printf("Conectando ao Wi-Fi...\n");

    /* Tenta conectar com WPA2-AES. Timeout de 30 s.
     * IMPORTANTE: substitua "REDE" e "SENHA" pelas credenciais reais antes da entrega. */
    if (cyw43_arch_wifi_connect_timeout_ms("REDE", "SENHA", CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("Erro ao conectar no Wi-Fi\n");
        return -1;
    }

    printf("Wi-Fi conectado\n");

    /* Inicializa o ADC e habilita o sensor de temperatura interno.
     * O sensor esta no canal 4 (canais 0-3 sao GPIOs fisicos). */
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4); // Canal 4 = sensor de temperatura da CPU

    /* Converte o endereco IP do broker de string para estrutura ip_addr_t. */
    ip_addr_t end_mqtt;
    ip4addr_aton(END_MQTT, &end_mqtt);

    /* Cria a estrutura do cliente MQTT na memoria dinamica do lwIP. */
    mqtt_client_t *mqtt_cliente = mqtt_client_new();
    if (mqtt_cliente == NULL) {
        printf("Falha ao criar cliente MQTT\n");
        return 1;
    }

    /* Registra os callbacks que serao chamados quando mensagens chegarem:
     *   - mqtt_recebendo_publicacao_cb: avisa que uma mensagem chegou (topico + tamanho)
     *   - mqtt_dados_recebidos_cb:      entrega o conteudo (payload) da mensagem */
    mqtt_set_inpub_callback(mqtt_cliente, mqtt_recebendo_publicacao_cb, mqtt_dados_recebidos_cb, NULL);

    /* Inicia a conexao TCP com o broker na porta 1883 (MQTT sem TLS).
     * mqtt_conectado_cb sera chamado quando a conexao for aceita ou recusada. */
    err_t status_conn = mqtt_client_connect(mqtt_cliente, &end_mqtt, 1883, mqtt_conectado_cb, NULL, &info_cliente);
    if (status_conn != ERR_OK) {
        printf("Falha na requisicao de conexao MQTT: %d\n", status_conn);
        return 1;
    }

    /* Garante que o LED comeca apagado. */
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);

    bool estado_led = false;                                          // Estado atual do LED (ligado/desligado)
    uint32_t ultima_borda_botao_ms = 0;                              // Ultima vez que o botao foi detectado
    uint32_t ultimo_toggle_led_ms = to_ms_since_boot(get_absolute_time()); // Ultima vez que o LED alterneu

    /* Loop principal — roda indefinidamente apos a inicializacao. */
    while (true) {
        /* Tempo atual em ms desde o boot — usado para medir intervalos sem travar o processador. */
        uint32_t agora_ms = to_ms_since_boot(get_absolute_time());

        /* --- BOTAO: publica temperatura via MQTT ---
         * board_button_read() retorna true enquanto o botao estiver pressionado.
         * O debounce garante que so processa uma vez a cada DEBOUNCE_MS milissegundos. */
        if (board_button_read() && ((agora_ms - ultima_borda_botao_ms) > DEBOUNCE_MS)) {
            ultima_borda_botao_ms = agora_ms;

            if (conectado_com_sucesso) {
                float temp_c = ler_temperatura_cpu_celsius();

                /* Formata a temperatura com 2 casas decimais, ex: "36.54" */
                char msg[48];
                int n = snprintf(msg, sizeof(msg), "%.2f", temp_c);

                if (n > 0) {
                    /* Publica no topico de temperatura.
                     * QoS 0 = sem confirmacao, retain 0 = nao armazena no broker. */
                    err_t pub_status = mqtt_publish(mqtt_cliente,
                                                    TOPICO_TEMPERATURA,
                                                    msg,
                                                    (u16_t)strlen(msg),
                                                    0,   // QoS
                                                    0,   // retain
                                                    mqtt_requisicao_cb,
                                                    NULL);
                    if (pub_status == ERR_OK) {
                        printf("Temperatura publicada em %s: %s C\n", TOPICO_TEMPERATURA, msg);
                    } else {
                        printf("Falha ao publicar temperatura (erro %d)\n", pub_status);
                    }
                }
            } else {
                printf("MQTT ainda nao conectado; publish ignorado\n");
            }
        }

        /* --- LED: pisca no intervalo definido via MQTT ---
         * Se intervalo_led_segundos > 0, alterna o LED a cada N segundos.
         * Caso contrario, mantem o LED apagado e reseta o temporizador. */
        if (intervalo_led_segundos > 0) {
            uint32_t intervalo_ms = (uint32_t)intervalo_led_segundos * 1000u;
            if ((agora_ms - ultimo_toggle_led_ms) >= intervalo_ms) {
                ultimo_toggle_led_ms = agora_ms;
                estado_led = !estado_led; // Inverte o estado
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, estado_led ? 1 : 0);
            }
        } else {
            /* Sem intervalo ativo: apaga o LED e reseta o contador para
             * evitar que o LED acenda imediatamente quando um novo intervalo for recebido. */
            estado_led = false;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            ultimo_toggle_led_ms = agora_ms;
        }

        /* Pausa de 50 ms — cede tempo para o stack de rede (lwIP) processar
         * pacotes TCP/IP recebidos, como mensagens MQTT chegando. */
        sleep_ms(50);
    }
}
