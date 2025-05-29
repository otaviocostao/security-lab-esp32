#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h> // Certifique-se que a biblioteca FirebaseClient by Mobizt está instalada
#include <SPI.h>
#include <MFRC522.h> // Certifique-se que a biblioteca MFRC522 by GithubCommunity está instalada
#include <ESP32Servo.h>

// --- Configurações ---

// Pinos do RFID (Ajuste conforme sua ligação)
#define RST_PIN 22
#define SS_PIN  21

// Credenciais Wi-Fi (Substitua pelas suas - VERIFIQUE CUIDADOSAMENTE!)
#define WIFI_SSID "Otica Nordeste"
#define WIFI_PASSWORD "nordeste5945"

// Credenciais Firebase (Substitua pelas suas - MANTENHA A API KEY E EMAIL/SENHA SEGUROS!)
#define API_KEY "AIzaSyAQOogYuOAKJt4irq17qvuOadGTA5dr08o" // Sua Web API Key do Firebase
#define DATABASE_URL "https://rfid-com-esp32-default-rtdb.firebaseio.com/" // Sua URL do Realtime Database
#define USER_EMAIL "rafafigolif@gmail.com" // Email de um usuário autenticado no seu projeto Firebase
#define USER_PASSWORD "123456" // Senha do usuário Firebase

// Definição do Prefixo do Dispositivo
const char* DEVICE_ID_PREFIX = "ESP32_"; // Defina o prefixo desejado aqui

// *** NOVAS CONSTANTES PARA INFORMAÇÕES DO DISPOSITIVO ***
#define FIRMWARE_VERSION "1.0.1" // Defina a versão atual do seu firmware
#define MOCKUP_RAM_USAGE 48       // Valor de exemplo para RAM
#define MOCKUP_TEMPERATURE 26     // Valor de exemplo para Temperatura
#define MOCKUP_LATENCY 55         // Valor de exemplo para Latência
#define MOCKUP_CPU_USAGE 32       // Valor de exemplo para CPU

// --- Pinos dos LEDs ---
#define LED_PORTA_FECHADA_PIN 12
#define LED_PORTA_ABERTA_PIN 14

// Constantes do Servo Motor
const int SERVO_PIN = 16;
const int ANGULO_FECHADO = 70; 
const int ANGULO_ABERTO = 130;
Servo servo;

// --- Objetos Globais ---

// RFID
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Firebase & Rede
typedef WiFiClientSecure SecureClient; // Usando WiFiClientSecure para HTTPS
SecureClient ssl_client;               // Objeto cliente SSL/TLS
FirebaseApp app;                       // Objeto da aplicação Firebase
AsyncClientClass aClient(ssl_client, true); // Cliente assíncrono para Firebase (true pode otimizar buffer)
RealtimeDatabase database;             // Objeto do Realtime Database (inicializado vazio)

// Identificador do Dispositivo
String deviceMAC = "";          // Guarda o MAC original (XX:XX:...)
String firebaseDeviceID = "";   // Guarda o ID FINAL do dispositivo (Prefixo + MAC formatado)
String devicePath = "";         // Guarda o caminho do dispositivo no Firebase: /devices/{firebaseDeviceID}

// Controle
bool firebaseReady = false;         // Flag para indicar se o Firebase está pronto
unsigned long lastRead = 0;         // Tempo da última tentativa de leitura RFID
const unsigned long readInterval = 2000; // Intervalo mínimo entre leituras (ms)

// --- Controle dos LEDs e Estado da Porta ---
unsigned long doorOpenTimerStart = 0;          // Timestamp de quando a porta foi aberta
const unsigned long doorOpenDuration = 2000; // Porta fica "aberta" por X segundos
bool isDoorOpen = false;                       // Flag para indicar se a porta está "aberta"

// --- Função para atualizar o status e last_status_change do dispositivo no Firebase ---
void updateDeviceStatusFirebase(const String& newStatus) {
    if (firebaseReady && !firebaseDeviceID.isEmpty() && WiFi.status() == WL_CONNECTED) {
        if (devicePath.isEmpty()) { // Recalcula se necessário
            devicePath = "/devices/" + firebaseDeviceID;
        }
        
        object_t statusUpdateJson;
        JsonWriter writer;

        // Placeholder para o timestamp do servidor
        object_t serverTimestampPlaceholder;
        JsonWriter tsWriter;
        tsWriter.create(serverTimestampPlaceholder, ".sv", "timestamp");

        // Criar os campos para o JSON de atualização
        object_t statusField, lastStatusChangeField;
        writer.create(statusField, "status", newStatus.c_str());
        writer.create(lastStatusChangeField, "last_status_change", serverTimestampPlaceholder); // MODIFICADO: Adiciona last_status_change

        // Juntar os campos no JSON final
        writer.join(statusUpdateJson, 2, statusField, lastStatusChangeField);

        Serial.printf("Tentando atualizar status para '%s' e last_status_change em %s\n", newStatus.c_str(), devicePath.c_str());
        if (database.update(aClient, devicePath, statusUpdateJson)) {
            Serial.printf("Status do dispositivo atualizado para '%s' e last_status_change no Firebase com sucesso.\n", newStatus.c_str());
        } else {
            Serial.printf(">> Erro ao atualizar status para '%s' e last_status_change no Firebase: %s (Code %d)\n",
                          newStatus.c_str(),
                          aClient.lastError().message().c_str(),
                          aClient.lastError().code());
        }
    } else {
        Serial.printf("Não foi possível atualizar status para '%s': Firebase não pronto, ID do dispositivo vazio ou WiFi desconectado.\n", newStatus.c_str());
    }
}


// --- Função Setup ---
void setup() {
  Serial.begin(115200);
  while (!Serial);
  delay(500);
  Serial.println("\n\nIniciando Setup...");

  // --- Configuração dos Pinos dos LEDs ---
  pinMode(LED_PORTA_FECHADA_PIN, OUTPUT);
  pinMode(LED_PORTA_ABERTA_PIN, OUTPUT);

  // Configura o Servo Motor
  servo.attach(SERVO_PIN);
  servo.write(ANGULO_FECHADO);


  digitalWrite(LED_PORTA_FECHADA_PIN, HIGH); // Inicialmente, porta fechada
  digitalWrite(LED_PORTA_ABERTA_PIN, LOW);  // LED de porta aberta apagado
  Serial.println("LEDs configurados: Porta Fechada (ON), Porta Aberta (OFF)");


  // --- Configuração Wi-Fi ---
  Serial.println("Configurando WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  // --- Scan de Redes ---
  Serial.println("Procurando redes Wi-Fi disponíveis...");
  int n = WiFi.scanNetworks();
  if (n == 0) {
    Serial.println("Nenhuma rede Wi-Fi encontrada!");
  } else {
    Serial.printf("%d rede(s) encontrada(s):\n", n);
    for (int i = 0; i < n; ++i) {
      Serial.printf("  %d: %s (%d dBm) %s\n",
                    i + 1,
                    WiFi.SSID(i).c_str(),
                    WiFi.RSSI(i),
                    WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "[Aberta]" : "[Segura]");
      delay(10);
    }
  }
  Serial.println("Scan concluído.");
  Serial.println();

  // --- Tentativa de Conexão ---
  Serial.printf("Tentando conectar a rede Wi-Fi '%s'...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startWifi = millis();
  int wifiStatus;
  Serial.print("Aguardando conexão");
  while ((wifiStatus = WiFi.status()) != WL_CONNECTED && millis() - startWifi < 15000) {
    Serial.print('.');
    digitalWrite(LED_PORTA_ABERTA_PIN, HIGH);
    delay(300);
    digitalWrite(LED_PORTA_ABERTA_PIN, LOW);
    delay(500);
  }
  Serial.println();

  // --- Verifica Resultado da Conexão e Obtém MAC Address ---
  if (wifiStatus == WL_CONNECTED) {
    Serial.printf("Conectado com sucesso a '%s'! Endereço IP: %s\n",
                  WIFI_SSID, WiFi.localIP().toString().c_str());

    // --- Obter e Formatar MAC Address e Adicionar Prefixo ---
    deviceMAC = WiFi.macAddress();
    String formattedMAC = deviceMAC;
    formattedMAC.replace(":", "");
    formattedMAC.toUpperCase();
    firebaseDeviceID = String(DEVICE_ID_PREFIX) + formattedMAC;
    devicePath = "/devices/" + firebaseDeviceID; // Define o devicePath globalmente
    
    digitalWrite(LED_PORTA_ABERTA_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PORTA_ABERTA_PIN, LOW);
    delay(500);
    digitalWrite(LED_PORTA_ABERTA_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PORTA_ABERTA_PIN, LOW);
    delay(500);
    digitalWrite(LED_PORTA_ABERTA_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PORTA_ABERTA_PIN, LOW); 

    Serial.printf("Endereço MAC (Original): %s\n", deviceMAC.c_str());
    Serial.printf("ID Final do Dispositivo para Firebase (com prefixo): %s\n", firebaseDeviceID.c_str());
    Serial.printf("Caminho do dispositivo no Firebase: %s\n", devicePath.c_str());


  } else {
    Serial.printf("Falha ao conectar ao Wi-Fi '%s'. Último status: %d\n", WIFI_SSID, wifiStatus);
    Serial.println("Reiniciando o ESP32 devido à falha na conexão Wi-Fi...");
    delay(5000);
    ESP.restart();
  }

  // --- Inicialização RFID ---
  Serial.println("Inicializando leitor RFID MFRC522...");
  SPI.begin();
  mfrc522.PCD_Init();
  delay(4);
  if (mfrc522.PCD_PerformSelfTest()) {
    Serial.println(F("Leitor RFID inicializado e passou no autoteste."));
  } else {
    Serial.println(F("Falha na inicialização do leitor RFID ou no autoteste!"));
    Serial.println(F("Verifique ligação e alimentação. Reiniciando em 10s..."));
    delay(10000);
    ESP.restart();
  }
  mfrc522.PCD_DumpVersionToSerial();

  // --- Inicialização Firebase ---
  Serial.println("Inicializando conexão com Firebase...");
  ssl_client.setInsecure(); // Para desenvolvimento. Em produção, use certificados.
  Serial.println("AVISO: Cliente SSL configurado como inseguro.");
  UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASSWORD);
  initializeApp(aClient, app, getAuth(user_auth));

  Serial.print("Aguardando autenticação do Firebase");
  unsigned long startAuth = millis();
  while (!app.ready() && millis() - startAuth < 30000) { // Timeout de 30 segundos para autenticação
    Serial.print('.');
    app.loop(); // Necessário para processar a autenticação assíncrona
    delay(500);
  }
  Serial.println();

  if (app.ready()) {
    Serial.println("Autenticação com Firebase realizada com sucesso!");
    app.getApp<RealtimeDatabase>(database);
    database.url(DATABASE_URL);
    firebaseReady = true;
    Serial.println("Instância do RealtimeDatabase obtida e URL configurada.");

    // --- INÍCIO: Lógica de Registro/Atualização do Dispositivo no Firebase ---
    if (!firebaseDeviceID.isEmpty()) {
        Serial.printf("Verificando/Registrando dispositivo no Firebase em: %s\n", devicePath.c_str());

        bool deviceExists = database.exists(aClient, devicePath);

        if (aClient.lastError().code() != 0) {
            Serial.printf(">> Erro ao verificar existência do dispositivo no Firebase: %s (Code %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
            Serial.println("Não será possível registrar ou atualizar o dispositivo neste momento.");
        } else {
            object_t timestampPlaceholder;
            JsonWriter tsWriter;
            tsWriter.create(timestampPlaceholder, ".sv", "timestamp");

            if (!deviceExists) {
                Serial.println("Dispositivo não encontrado. Registrando novo dispositivo...");
                object_t actionReqField, fwField, lastOnlineField, statusField, lastStatusChangeField, ramField, tempField, latencyField, cpuField, newDeviceJson; // Adicionado lastStatusChangeField
                JsonWriter writer;

                writer.create(actionReqField, "action_requested_at", timestampPlaceholder);
                writer.create(fwField, "firmware_version", FIRMWARE_VERSION);
                writer.create(lastOnlineField, "last_online", timestampPlaceholder);
                writer.create(statusField, "status", "locked"); 
                writer.create(lastStatusChangeField, "last_status_change", timestampPlaceholder); // MODIFICADO: Adiciona last_status_change
                writer.create(ramField, "ram_usage", MOCKUP_RAM_USAGE);
                writer.create(tempField, "temperature", MOCKUP_TEMPERATURE);
                writer.create(latencyField, "latency", MOCKUP_LATENCY);
                writer.create(cpuField, "cpu_usage", MOCKUP_CPU_USAGE);
                writer.join(newDeviceJson, 9, actionReqField, fwField, lastOnlineField, statusField, lastStatusChangeField, ramField, tempField, latencyField, cpuField); // Contagem atualizada para 9

                Serial.println("Enviando dados do novo dispositivo...");
                bool creationSuccess = database.set<object_t>(aClient, devicePath, newDeviceJson);

                if (creationSuccess) {
                    Serial.println("Novo dispositivo registrado com sucesso no Firebase (status 'locked', last_status_change atualizado).");
                } else {
                    Serial.printf(">> Erro ao registrar novo dispositivo no Firebase: %s (Code %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
                }
            } else {
                Serial.println("Dispositivo já registrado. Atualizando informações...");
                
                object_t updateData;
                JsonWriter writer;
                object_t lastOnlineField, statusField, lastStatusChangeField, ramField, tempField, latencyField, cpuField, fwField; // Adicionado lastStatusChangeField

                writer.create(lastOnlineField, "last_online", timestampPlaceholder);
                writer.create(statusField, "status", "locked");
                writer.create(lastStatusChangeField, "last_status_change", timestampPlaceholder); // MODIFICADO: Adiciona last_status_change
                writer.create(ramField, "ram_usage", MOCKUP_RAM_USAGE);
                writer.create(tempField, "temperature", MOCKUP_TEMPERATURE);
                writer.create(latencyField, "latency", MOCKUP_LATENCY);
                writer.create(cpuField, "cpu_usage", MOCKUP_CPU_USAGE);
                writer.create(fwField, "firmware_version", FIRMWARE_VERSION);

                writer.join(updateData, 8, lastOnlineField, statusField, lastStatusChangeField, ramField, tempField, latencyField, cpuField, fwField); // Contagem atualizada para 8

                if (database.update(aClient, devicePath, updateData)) {
                     Serial.println("Informações do dispositivo (status 'locked', last_status_change, etc.) atualizadas com sucesso.");
                } else {
                    Serial.printf(">> Falha ao atualizar informações do dispositivo: %s (Code %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
                }
            }
        }
    } else {
        Serial.println("AVISO: ID do dispositivo (MAC) vazio. Não é possível registrar ou atualizar o dispositivo no Firebase.");
    }
    // --- FIM: Lógica de Registro/Atualização do Dispositivo no Firebase ---

  } else {
    Serial.println("Falha na autenticação com Firebase ou app não está pronto!");
    if (aClient.lastError().code() != 0) {
      Serial.printf("Erro Firebase (cliente durante inicialização/autenticação): %s (Código: %d)\n",
                    aClient.lastError().message().c_str(),
                    aClient.lastError().code());
    } else {
      Serial.println("App não ficou pronto no tempo esperado, mas o cliente não reportou erro específico. Verifique logs ou tempo de expiração da autenticação.");
    }
    firebaseReady = false;
    Serial.println("Não será possível operar com o Firebase. Verifique credenciais, regras de segurança, conexão e tempo de expiração da autenticação.");
  }

  Serial.println(F("\nSetup concluído. Sistema pronto."));
  Serial.println(F("Aproxime o cartão ou tag RFID..."));

} // Fim do setup()

// --- Função Loop ---
void loop() {
  app.loop(); // Manter a conexão e tarefas assíncronas do Firebase

  // --- Gerenciamento do estado da porta e LEDs ---
  if (isDoorOpen && (millis() - doorOpenTimerStart >= doorOpenDuration)) {
    Serial.println("Tempo de porta aberta expirou. Fechando porta...");
    digitalWrite(LED_PORTA_ABERTA_PIN, LOW);
    digitalWrite(LED_PORTA_FECHADA_PIN, HIGH);

    servo.write(ANGULO_FECHADO);
    isDoorOpen = false;
    doorOpenTimerStart = 0; // Reset timer
    updateDeviceStatusFirebase("locked"); 
  }


  if (millis() - lastRead < readInterval) {
    return; // Evita leituras muito frequentes
  }
  lastRead = millis();

  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    if (!isDoorOpen) {
         digitalWrite(LED_PORTA_FECHADA_PIN, HIGH); 
         digitalWrite(LED_PORTA_ABERTA_PIN, LOW);  
         servo.write(ANGULO_FECHADO);
    }
    delay(50); 
    return;
  }

  // --- Cartão Detectado e Lido ---
  Serial.println(F("\n--- Cartão Detectado ---"));

  String cardUID_for_path = "";
  String cardUID_for_value = ""; 
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) cardUID_for_path += "0";
    cardUID_for_path += String(mfrc522.uid.uidByte[i], HEX);

    cardUID_for_value += String(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
    cardUID_for_value += String(mfrc522.uid.uidByte[i], HEX);
  }
  cardUID_for_path.toUpperCase();
  cardUID_for_value.toUpperCase();
  cardUID_for_value.trim();
  Serial.printf("UID Cartão (p/ caminho Firebase): %s\n", cardUID_for_path.c_str());
  Serial.printf("UID Cartão (p/ valor - log visual): %s\n", cardUID_for_value.c_str());

  String block4Data = "";
  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF; 
  byte blockAddr = 4;
  byte bufferSize = 18; 
  byte readBuffer[bufferSize];
  MFRC522::StatusCode status_rfid; // Renomeado para evitar conflito com status da porta

  Serial.printf("Autenticando Bloco %d...\n", blockAddr);
  status_rfid = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &(mfrc522.uid));

  if (status_rfid == MFRC522::STATUS_OK) {
    Serial.println("Autenticação OK.");
    Serial.printf("Lendo Bloco %d...\n", blockAddr);
    status_rfid = mfrc522.MIFARE_Read(blockAddr, readBuffer, &bufferSize);
    if (status_rfid == MFRC522::STATUS_OK) {
      Serial.print(F("Dados Lidos do Bloco 4 (Hex):"));
      for (uint8_t i = 0; i < 16; i++) { 
        Serial.printf(readBuffer[i] < 0x10 ? " 0%X" : " %X", readBuffer[i]);
        if (readBuffer[i] >= 0x20 && readBuffer[i] <= 0x7E) { 
          block4Data += (char)readBuffer[i];
        }
      }
      block4Data.trim(); 
      Serial.println();
      if (block4Data.length() > 0) {
        Serial.printf("Dados convertidos para String (Bloco 4): '%s'\n", block4Data.c_str());
      } else {
        Serial.println("Nenhum caractere ASCII imprimível encontrado no Bloco 4.");
      }
    } else {
      Serial.print(F("Falha na leitura do Bloco 4: ")); Serial.println(mfrc522.GetStatusCodeName(status_rfid));
    }
  } else {
    Serial.print(F("Falha na autenticação do Bloco 4: ")); Serial.println(mfrc522.GetStatusCodeName(status_rfid));
  }

  // --- Interação com Firebase ---
  if (firebaseReady && WiFi.status() == WL_CONNECTED && !firebaseDeviceID.isEmpty()) {

    bool cardIsEffectivelyAuthorized = false; 

    String authPathFb = "/devices/" + firebaseDeviceID + "/authorized_tags/" + cardUID_for_path; 
    Serial.printf("Verificando autorização do cartão em Firebase: %s\n", authPathFb.c_str());

    bool isAuthorizedFirebaseValue = database.get<bool>(aClient, authPathFb);

    if (aClient.lastError().code() == 0) {
        if (isAuthorizedFirebaseValue) { 
            Serial.println(">>> Porta Aberta! Acesso Autorizado. <<<");
            cardIsEffectivelyAuthorized = true;
        } else {
            Serial.println(">>> Acesso Negado! Cartão não encontrado em 'authorized_tags' ou valor é 'false'. <<<");
            cardIsEffectivelyAuthorized = false;
        }
    } else {
        Serial.printf(">>> Acesso Negado! Erro ao verificar autorização do cartão no Firebase (Code: %d): %s\n",
                      aClient.lastError().code(),
                      aClient.lastError().message().c_str());
        Serial.println("Verifique se o cartão está cadastrado em 'authorized_tags' com valor 'true' ou se há problemas de permissão/conexão.");
        cardIsEffectivelyAuthorized = false;
    }

    if (cardIsEffectivelyAuthorized) {
        Serial.println("ACIONANDO LEDs e SERVO: Abrindo porta...");
        digitalWrite(LED_PORTA_ABERTA_PIN, HIGH);
        digitalWrite(LED_PORTA_FECHADA_PIN, LOW);
         
        servo.write(ANGULO_ABERTO);
        isDoorOpen = true;
        doorOpenTimerStart = millis(); 
        updateDeviceStatusFirebase("unlocked"); 
    } else {
        if (!isDoorOpen) { 
            Serial.println("ACIONANDO LEDs e SERVO: Porta permanece fechada (acesso negado).");
            digitalWrite(LED_PORTA_ABERTA_PIN, LOW);
            digitalWrite(LED_PORTA_FECHADA_PIN, HIGH);
            servo.write(ANGULO_FECHADO);
            updateDeviceStatusFirebase("locked"); 
        } else {
            Serial.println("ACIONANDO LEDs: Acesso negado, mas porta já estava aberta (timer). Nenhuma mudança imediata nos LEDs/Servo/Status Firebase.");
        }
    }


    // --- ETAPA 1: Verificação e Criação de Usuário no nó /users (se não existir) ---
    String userPath = "/users/" + cardUID_for_path;
    Serial.printf("Verificando/Criando usuário em Firebase: %s\n", userPath.c_str());

    bool userExists = database.exists(aClient, userPath);

    if (aClient.lastError().code() != 0) {
        Serial.printf(">> Erro ao verificar existência do usuário no Firebase: %s (Code %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
    } else {
        if (!userExists) {
            Serial.println("Usuário não encontrado no Firebase. Criando novo registro de usuário...");

            object_t timestampPlaceholderUser, statusFieldUser, roleFieldUser, createdAtFieldUser, rfidTagIdFieldUser, newUserJson; 
            JsonWriter userWriter;

            userWriter.create(timestampPlaceholderUser, ".sv", "timestamp");
            userWriter.create(statusFieldUser, "status", "inactive"); 
            userWriter.create(roleFieldUser, "role", "new_card");   
            userWriter.create(createdAtFieldUser, "created_at", timestampPlaceholderUser);
            userWriter.create(rfidTagIdFieldUser, "rfidTagId", cardUID_for_path.c_str()); 

            userWriter.join(newUserJson, 4, statusFieldUser, roleFieldUser, createdAtFieldUser, rfidTagIdFieldUser);

            Serial.printf("Enviando dados do novo usuário para: %s\n", userPath.c_str());
            bool creationSuccessUser = database.set<object_t>(aClient, userPath, newUserJson);

            if (creationSuccessUser) {
                Serial.println("Novo usuário criado com sucesso no Firebase (com rfidTagId).");
            } else {
                Serial.printf(">> Erro ao criar novo usuário no Firebase: %s (Code %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
            }
        } else {
            Serial.println("Usuário já existe no Firebase. Nenhuma ação de criação de usuário necessária.");
        }
    }

    // --- ETAPA 2: Log da Leitura no nó /leituras_por_dispositivo/ ---
    String firebasePathLogBase = "/leituras_por_dispositivo/" + firebaseDeviceID + "/" + cardUID_for_path;
    String logEntryPath = database.push(aClient, firebasePathLogBase, ""); 

    if (aClient.lastError().code() == 0 && !logEntryPath.isEmpty()) {
        Serial.printf("Registrando log de leitura em Firebase sob: %s\n", logEntryPath.c_str());
        bool logSuccess = true;

        if (!database.set(aClient, logEntryPath + "/uid_cartao", cardUID_for_path.c_str())) {
          Serial.printf(">> Erro ao enviar UID Cartão (log): %s (Code %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
          logSuccess = false;
        }
        if (!database.set(aClient, logEntryPath + "/timestamp_leitura_ms", (unsigned long)millis())) {
          Serial.printf(">> Erro ao enviar Timestamp (log): %s (Code %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
          logSuccess = false;
        }
        object_t serverTimestampLog;
        JsonWriter tsLogWriter;
        tsLogWriter.create(serverTimestampLog, ".sv", "timestamp");
        if (!database.set<object_t>(aClient, logEntryPath + "/timestamp_servidor", serverTimestampLog)) {
            Serial.printf(">> Erro ao enviar Timestamp do Servidor (log): %s (Code %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
            logSuccess = false;
        }
        if (!database.set(aClient, logEntryPath + "/mac_dispositivo", deviceMAC.c_str())) {
           Serial.printf(">> Erro ao enviar MAC (log): %s (Code %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
           logSuccess = false;
        }
        if (!block4Data.isEmpty()) {
          if (!database.set(aClient, logEntryPath + "/dados_bloco_4", block4Data.c_str())) {
            Serial.printf(">> Erro ao enviar Bloco 4 (log): %s (Code %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
            logSuccess = false;
          }
        } else {
           Serial.println("(Sem dados do Bloco 4 para enviar ao log)");
        }
        if (!database.set(aClient, logEntryPath + "/acesso_concedido", cardIsEffectivelyAuthorized)) {
            Serial.printf(">> Erro ao enviar status de acesso (log): %s (Code %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
            logSuccess = false;
        }

        if (logSuccess) {
           Serial.println("Log de leitura enviado ao Firebase com sucesso.");
        } else {
           Serial.println("Falhas ao enviar alguns dados do log de leitura ao Firebase.");
        }
    } else {
        Serial.printf(">> Erro ao criar entrada de log (push) no Firebase: %s (Code %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
        Serial.println("Não foi possível registrar o log da leitura.");
    }

  } else if (firebaseDeviceID.isEmpty() && firebaseReady) {
    Serial.println(F("AVISO: ID do dispositivo (MAC) não obtido. Não é possível interagir com Firebase."));
  } else if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("AVISO: WiFi desconectado. Não é possível interagir com Firebase."));
  } else { // firebaseReady == false
    Serial.println(F("AVISO: Firebase não está pronto. Não é possível interagir com Firebase."));
  }

  // --- Finalização da Leitura RFID ---
  mfrc522.PICC_HaltA();      
  mfrc522.PCD_StopCrypto1(); 
  Serial.println(F("--- Fim da Leitura do Cartão ---"));

} // Fim do loop()