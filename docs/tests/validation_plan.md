# Software Verification & Validation Plan (SVVP)


## Visão Geral
Este documento rastreia os Casos de Teste (TC) funcionais e de resiliência necessários para validar o firmware nos 4 modos operacionais da FSM.

---

## Fases 2 e 3: Hardware, RTOS e Baseline de Armazenamento
**Status:** EM ANDAMENTO

### Testes Funcionais Essenciais
* **TC-01: Aquisição de Sensor (ADC1)**
  * **Objetivo:** Verificar a amostragem periódica (ex: 30s) utilizando exclusivamente o ADC1, sem conflito com o Wi-Fi.
  * **Resultado Esperado:** Leituras estáveis capturadas sem crash no driver do ADC.

* **TC-02: Feedback Visual da FSM**
  * **Objetivo:** Garantir que o LED de status e o display OLED reflitam as transições da FSM em ≤ 200 ms.
  * **Resultado Esperado:** O OLED atualiza a string do estado; o LED assume o padrão correto (ex: sólido para OPERAÇÃO, pisca rápido para RESSINCRONIZAÇÃO).

* **TC-03: Integridade LittleFS pós Power-Loss**
  * **Objetivo:** Verificar se nenhum registro é corrompido em caso de corte de energia durante a gravação (validação do Copy-on-Write).
  * **Resultado Esperado:** O sistema se recupera graciosamente após 10 cortes de energia aleatórios; sem perda dos dados já comitados no buffer.

### Testes de Conectividade e Resiliência
* **TC-04: Reconexão Wi-Fi e Backoff Exponencial**
  * **Objetivo:** Testar o comportamento do sistema quando o roteador é desligado.
  * **Resultado Esperado:** Transição imediata para EMERGÊNCIA e reconexão bem-sucedida em ≤ 60s após a volta do roteador.

* **TC-05: Ressincronização Histórica (Store-and-Forward)**
  * **Objetivo:** Validar o dreno do buffer LittleFS após a restauração da rede.
  * **Resultado Esperado:** Os dados acumulados são publicados em ordem cronológica com a flag `historic: true`. Nenhuma duplicata gerada (garantia via QoS 1 / PUBACK).

* **TC-08: Configuração Inicial via Portal Cativo**
  * **Objetivo:** Verificar o fallback para SoftAP quando a NVS está vazia.
  * **Resultado Esperado:** O técnico de campo consegue conectar ao SSID "resilient-iot-cfg", inserir as credenciais no portal HTTP em ≤ 3 minutos, e o dispositivo reinicia no modo OPERAÇÃO.

### Desempenho e Estabilidade
* **TC-10: Memory Leak e Uptime**
  * **Objetivo:** Monitorar a estabilidade do sistema em uma execução contínua de 72 horas.
  * **Resultado Esperado:** Zero acionamentos do Task Watchdog. Memory leak ≤ 1 KB/hora.