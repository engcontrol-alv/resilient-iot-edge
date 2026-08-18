#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include "esp_err.h"

// Função para iniciar a conexão com o broker Mosquitto
esp_err_t mqtt_manager_init(void);

#endif // MQTT_MANAGER_H