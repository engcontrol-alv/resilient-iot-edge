#include "mqtt_manager.h"
#include "mqtt_client.h"
#include "esp_log.h"

static const char *TAG = "MQTT_MANAGER";

// Callback que lida com os eventos do MQTT (Conexão, Desconexão, Mensagens recebidas)
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Conectado ao Broker Mosquitto com sucesso!");
            // Aqui futuramente faremos o esp_mqtt_client_subscribe
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Desconectado do Broker.");
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Mensagem recebida no tópico: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Dados: %.*s", event->data_len, event->data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "Erro no MQTT!");
            break;
        default:
            break;
    }
}

esp_err_t mqtt_manager_init(void) {
    /* 
     * ATENÇÃO ÁLVARO:
     * Substitua o IP abaixo pelo número que você achou no 'ipconfig'
     */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://192.168.15.6:1883", 
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Falha ao inicializar o cliente MQTT");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    return ESP_OK;
}