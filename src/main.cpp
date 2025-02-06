#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <MFRC522.h>
#include <LiquidCrystal_I2C.h>
#include "BluetoothSerial.h"

// ===== Configurações de Pinos =====
#define RFID_CS_PIN     33    // CS do leitor RFID
#define RFID_RST_PIN    5     // RST do leitor RFID
#define SD_CS_PIN       32    // Pino CS do módulo SD
#define BUTTON_PIN      13     // Botão (trava de operação)
#define PIN_VERDE       14    // LED verde
#define PIN_VERMELHO    14    // LED vermelho

// ===== Configurações do LCD =====
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ===== Configurações do Bluetooth =====
BluetoothSerial SerialBT;

// ===== Outras Definições =====
#define SD_FILE_NAME    "/gabaritos.dat"
#define MAX_GABARITOS   150
#define MAX_PECA        10
#define UID_SIZE        4
#define MAX_NAME_LENGTH 20

// ===== Estrutura para Associações =====
typedef struct {
  byte silkUID[MAX_PECA][UID_SIZE];        // UIDs das peças associadas
  char silkNames[MAX_PECA][MAX_NAME_LENGTH]; // Nomes das peças
  uint8_t totalSilks;                        // Quantidade de peças cadastradas
} GabaritoAssociacao;

// ===== Variáveis Globais =====
byte gabaritosUID[MAX_GABARITOS][UID_SIZE];
GabaritoAssociacao associacoes[MAX_GABARITOS];
int totalGabaritos = 0;

byte masterUID[UID_SIZE];
bool masterCadastrado = false;
bool autenticado = false;
bool modoCadastroAtivo = false;
volatile bool interruptFlag = false;

// ===== Objeto do Leitor RFID =====
MFRC522 mfrc522(RFID_CS_PIN, RFID_RST_PIN);

// ===== Funções de Exibição =====
void exibirStatus(String msg1, String msg2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(msg1);
  lcd.setCursor(0, 1);
  lcd.print(msg2);
  Serial.print("[STATUS] ");
  Serial.print(msg1);
  Serial.print(" - ");
  Serial.println(msg2);
}

void exibirStatusBT(String msg1, String msg2) {
  SerialBT.println(msg1);
  if (msg2 != "") {
    SerialBT.println(msg2);
  }
}

// ===== Funções Auxiliares =====
bool compararUID(byte *uid1, byte *uid2) {
  for (byte i = 0; i < UID_SIZE; i++) {
    if (uid1[i] != uid2[i])
      return false;
  }
  return true;
}

String converterUIDParaString(byte *uid, byte tamanho) {
  String uidString = "";
  for (byte i = 0; i < tamanho; i++) {
    if (uid[i] < 0x10)
      uidString += "0";
    uidString += String(uid[i], HEX);
  }
  uidString.toUpperCase();
  return uidString;
}

// ===== Funções de Armazenamento no SD =====
void salvarNoSD() {
  if (SD.exists(SD_FILE_NAME)) {
    SD.remove(SD_FILE_NAME);
  }
  
  File file = SD.open(SD_FILE_NAME, FILE_WRITE);
  if (!file) {
    Serial.println("Erro ao abrir arquivo para salvar!");
    return;
  }
  
  file.write((uint8_t*)masterUID, UID_SIZE);
  file.write((uint8_t*)&totalGabaritos, sizeof(totalGabaritos));
  
  for (int i = 0; i < totalGabaritos; i++) {
    file.write(gabaritosUID[i], UID_SIZE);
    file.write((uint8_t*)&associacoes[i].totalSilks, sizeof(associacoes[i].totalSilks));
    for (int j = 0; j < associacoes[i].totalSilks; j++) {
      file.write(associacoes[i].silkUID[j], UID_SIZE);
      file.write((uint8_t*)associacoes[i].silkNames[j], MAX_NAME_LENGTH);
    }
  }
  file.close();
  Serial.println("Dados salvos no SD!");
  exibirStatusBT("Dados salvos", "SD atualizado");
}

void carregarDoSD() {
  if (!SD.exists(SD_FILE_NAME)) {
    Serial.println("Arquivo SD não existe. Criando novo arquivo...");
    File file = SD.open(SD_FILE_NAME, FILE_WRITE);
    if (file) {
      file.close();
      Serial.println("Arquivo criado com sucesso.");
    } else {
      Serial.println("Erro ao criar o arquivo.");
    }
    return;
  }
  // Continua com a leitura se o arquivo existir
  File file = SD.open(SD_FILE_NAME, FILE_READ);
  if (!file) {
    Serial.println("Erro ao abrir arquivo SD para leitura.");
    return;
  }
  
  file.read((uint8_t*)masterUID, UID_SIZE);
  bool uidVazio = true;
  for (int i = 0; i < UID_SIZE; i++) {
    if (masterUID[i] != 0xFF && masterUID[i] != 0x00) {
      uidVazio = false;
      break;
    }
  }
  masterCadastrado = !uidVazio;
  
  file.read((uint8_t*)&totalGabaritos, sizeof(totalGabaritos));
  for (int i = 0; i < totalGabaritos; i++) {
    file.read(gabaritosUID[i], UID_SIZE);
    file.read((uint8_t*)&associacoes[i].totalSilks, sizeof(associacoes[i].totalSilks));
    for (int j = 0; j < associacoes[i].totalSilks; j++) {
      file.read(associacoes[i].silkUID[j], UID_SIZE);
      file.read((uint8_t*)associacoes[i].silkNames[j], MAX_NAME_LENGTH);
    }
  }
  file.close();
  Serial.println("Dados carregados do SD!");
  exibirStatusBT("Dados carregados", "SD pronto");
}

void limparSD() {
  if (SD.exists(SD_FILE_NAME)) {
    SD.remove(SD_FILE_NAME);
    totalGabaritos = 0;
    Serial.println("SD limpo: Todos os dados apagados.");
    exibirStatusBT("SD Limpo", "Todos os dados apagados");
  } else {
    Serial.println("Arquivo SD não existe.");
  }
}

// ===== Funções de Cartão Master =====
void salvarCartaoMaster(byte *uid) {
  memcpy(masterUID, uid, UID_SIZE);
  masterCadastrado = true;
  salvarNoSD();
}

void cadastrarCartaoMaster() {
  exibirStatus("Cadastro Master", "Aproxime o cartão");
  while (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    delay(50);
  }
  salvarCartaoMaster(mfrc522.uid.uidByte);
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  exibirStatus("Cartão Master", "Cadastrado");
  exibirStatusBT("Cartão Master", "Cadastrado com sucesso");
}

// ===== Função de Cadastro de Gabarito e Peças =====
void cadastrarEtiqueta() {
  exibirStatus("Modo Cadastro", "Leia o gabarito");
  Serial.println("Aproxime o cartão gabarito...");
  
  while (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    delay(10);
  }
  
  if (totalGabaritos < MAX_GABARITOS) {
    memcpy(gabaritosUID[totalGabaritos], mfrc522.uid.uidByte, UID_SIZE);
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    
    associacoes[totalGabaritos].totalSilks = 0;
    exibirStatus("Gabarito lido", "Cadastre as peças");
    exibirStatusBT("Gabarito lido", "Aproxime as peças ou envie UID via app");

    while (associacoes[totalGabaritos].totalSilks < MAX_PECA) {
      
      // ==== Leitura via Bluetooth ====
      if (SerialBT.available()) {
        String comando = SerialBT.readStringUntil('\n');
        comando.trim();

        if (comando.equalsIgnoreCase("fim")) break;  // Encerra cadastro via app

        if (comando.length() == UID_SIZE * 2) {  // Verifica se o UID está no formato correto
          byte tempUID[UID_SIZE];
          for (int i = 0; i < UID_SIZE; i++) {
            String byteStr = comando.substring(i * 2, i * 2 + 2);
            tempUID[i] = (byte) strtol(byteStr.c_str(), NULL, 16);
          }
          int index = associacoes[totalGabaritos].totalSilks;
          memcpy(associacoes[totalGabaritos].silkUID[index], tempUID, UID_SIZE);
          SerialBT.println("Digite o nome da peça:");
          while (!SerialBT.available()) { delay(10); }
          String nomeSilk = SerialBT.readStringUntil('\n');
          nomeSilk.trim();
          nomeSilk.toCharArray(associacoes[totalGabaritos].silkNames[index], MAX_NAME_LENGTH);
          associacoes[totalGabaritos].totalSilks++;

          exibirStatus("Peça cadastrada", nomeSilk);
          exibirStatusBT("Peça cadastrada:", nomeSilk);
        }
      }

      // ==== Leitura via RFID ====
      if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
        int index = associacoes[totalGabaritos].totalSilks;
        memcpy(associacoes[totalGabaritos].silkUID[index], mfrc522.uid.uidByte, UID_SIZE);
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();

        exibirStatus("Peça lida RFID", "Digite nome no app");
        exibirStatusBT("Peça lida via RFID", "Digite o nome da peça:");

        // Espera o nome da peça via Bluetooth
        while (!SerialBT.available()) { delay(10); }
        String nomeSilk = SerialBT.readStringUntil('\n');
        nomeSilk.trim();
        nomeSilk.toCharArray(associacoes[totalGabaritos].silkNames[index], MAX_NAME_LENGTH);
        associacoes[totalGabaritos].totalSilks++;

        exibirStatus("Peça cadastrada", nomeSilk);
        exibirStatusBT("Peça cadastrada:", nomeSilk);
      }

      delay(10);
    }
    
    if (associacoes[totalGabaritos].totalSilks > 0) {
      totalGabaritos++;
      salvarNoSD();
      exibirStatus("Cadastro Concluído", "Gabarito cadastrado");
      exibirStatusBT("Cadastro concluído", "Gabarito salvo");
    } else {
      exibirStatus("Erro", "Mínimo 1 peça");
      exibirStatusBT("Erro", "Cadastre ao menos 1 peça");
    }
  } else {
    exibirStatus("Limite atingido", "Sem espaço para mais gabaritos");
    exibirStatusBT("Erro", "Limite máximo de gabaritos atingido");
  }
}


// ===== Função de Operação =====
void modoOperacao() {
  if (totalGabaritos == 0) {
    exibirStatus("Lista vazia", "Cadastre um gabarito");
    exibirStatusBT("Lista vazia", "Cadastre um gabarito");
    delay(1000);
    return;
  }
  
  Serial.println("Aproxime o cartão gabarito para operação...");
  while (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    delay(10);
  }
  
  int indiceGabarito = -1;
  for (int i = 0; i < totalGabaritos; i++) {
    if (compararUID(gabaritosUID[i], mfrc522.uid.uidByte)) {
      indiceGabarito = i;
      break;
    }
  }
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  
  if (indiceGabarito != -1) {
    exibirStatus("Gabarito reconhecido", "Aproxime as peças");
    exibirStatusBT("Gabarito reconhecido", "Aproxime todas as peças");

    bool pecasLidas[MAX_PECA] = {false};  // Controle de peças lidas
    int totalLidas = 0;
    unsigned long inicio = millis();
    const unsigned long tempoLimite = 60000; // 1 minuto para leitura das peças

    while (millis() - inicio < tempoLimite) {
      // Leitura via RFID
      if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
        bool pecaEncontrada = false;
        for (int k = 0; k < associacoes[indiceGabarito].totalSilks; k++) {
          if (compararUID(associacoes[indiceGabarito].silkUID[k], mfrc522.uid.uidByte)) {
            if (!pecasLidas[k]) {
              pecasLidas[k] = true;
              totalLidas++;
              String nomePeca = associacoes[indiceGabarito].silkNames[k];
              exibirStatus("Peça correta", nomePeca);
              exibirStatusBT("Peça lida:", nomePeca);
              Serial.print("Peça correta lida: ");
              Serial.println(nomePeca);
            } else {
              exibirStatus("Peça já lida", associacoes[indiceGabarito].silkNames[k]);
              exibirStatusBT("Peça já lida:", associacoes[indiceGabarito].silkNames[k]);
            }
            pecaEncontrada = true;
            break;
          }
        }
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();

        if (!pecaEncontrada) {
          exibirStatus("Peça incorreta", "Tente novamente");
          exibirStatusBT("Peça incorreta", "Tente novamente");
        }
      }

      // Verifica se todas as peças foram lidas
      if (totalLidas == associacoes[indiceGabarito].totalSilks) {
        exibirStatus("Todas as peças OK", "Liberação concluída");
        exibirStatusBT("Sucesso", "Todas as peças lidas");
        digitalWrite(PIN_VERDE, HIGH);

        // Mantém o LED aceso enquanto o botão estiver pressionado
        while (digitalRead(BUTTON_PIN) == HIGH) {
          delay(100);
        }

        // Quando o botão for solto, resetar
        digitalWrite(PIN_VERDE, LOW);
        exibirStatus("Peça removida", "Leia o gabarito novamente");
        exibirStatusBT("Peça removida", "Reinicie o processo");
        return;  // Sai da função e volta ao início
      }

      // Mostra quantas peças faltam
      int faltam = associacoes[indiceGabarito].totalSilks - totalLidas;
      exibirStatus("Peças restantes:", String(faltam));
      exibirStatusBT("Peças restantes:", String(faltam));

      delay(10);
    }

    // Se o tempo acabar antes de ler todas as peças
    if (totalLidas < associacoes[indiceGabarito].totalSilks) {
      exibirStatus("Tempo esgotado", "Leia o gabarito novamente");
      exibirStatusBT("Tempo esgotado", "Nem todas as peças foram lidas");
    }

  } else {
    exibirStatus("Erro", "Gabarito não reconhecido");
    exibirStatusBT("Erro", "Gabarito não reconhecido");
  }
}

// ===== Setup e Loop Principal =====
void setup() {
  Serial.begin(9600);
  SerialBT.begin("Assinatura_Verde");
  Serial.println("Inicializando...");

  lcd.init();
  lcd.backlight();

  pinMode(RFID_CS_PIN, OUTPUT);
  digitalWrite(RFID_CS_PIN, HIGH);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(PIN_VERDE, OUTPUT_OPEN_DRAIN);
  pinMode(PIN_VERMELHO, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_VERMELHO, HIGH);

  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("Erro ao iniciar SD!");
  } else {
    Serial.println("SD iniciado com sucesso.");
  }
  
  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("Leitor RFID inicializado.");

  carregarDoSD();

  if (!masterCadastrado) {
    cadastrarCartaoMaster();
  } else {
    exibirStatus("Sistema iniciado", "Cartão master OK");
    exibirStatusBT("Sistema iniciado", "Cartão master OK");
  }
}

void loop() {
  if (SerialBT.available()) {
    String comando = SerialBT.readStringUntil('\n');
    comando.trim();

    if (comando.equalsIgnoreCase("modo cadastro")) {
      modoCadastroAtivo = true;
      autenticado = false;
      exibirStatus("Modo Cadastro", "Aproxime o master");
      exibirStatusBT("Modo Cadastro", "Aguardando autenticação");
    } else if (comando.equalsIgnoreCase("modo operacao")) {
      modoCadastroAtivo = false;
      autenticado = false;
      exibirStatus("Modo Operação", "Ativado");
      exibirStatusBT("Modo Operação", "Ativado");
    } else if (comando.startsWith("apagar")) {
      int indice = comando.substring(7).toInt() - 1;
      if (indice >= 0 && indice < totalGabaritos) {
        for (int i = indice; i < totalGabaritos - 1; i++) {
          memcpy(gabaritosUID[i], gabaritosUID[i + 1], UID_SIZE);
          associacoes[i] = associacoes[i + 1];
        }
        totalGabaritos--;
        salvarNoSD();
        exibirStatus("Exclusão", "Gabarito apagado");
        exibirStatusBT("Gabarito apagado", "");
      } else {
        exibirStatusBT("Erro", "Índice inválido para exclusão");
      }
    } else if (comando.equalsIgnoreCase("limpar")) {
      limparSD();
    } else {
      exibirStatusBT("Comando não reconhecido", "");
    }
  }

  if (modoCadastroAtivo) {
    if (!autenticado) {
      if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
        if (compararUID(mfrc522.uid.uidByte, masterUID)) {
          autenticado = true;
          exibirStatus("Modo Cadastro", "Autenticado");
          exibirStatusBT("Modo Cadastro", "Autenticado");
        } else {
          exibirStatus("Erro", "Cartão master inválido");
          exibirStatusBT("Erro", "Cartão master inválido");
        }
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();
      }
    } else {
      cadastrarEtiqueta();
    }
  } else {
    modoOperacao();
  }

  delay(10);
}
