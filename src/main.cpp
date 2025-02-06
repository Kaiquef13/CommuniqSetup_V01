#include <Arduino.h>
#include <MFRC522.h>
#include <SPI.h>
#include <EEPROM.h>
#include "BluetoothSerial.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>

#define RX_PIN 16  // Pino do Arduino conectado ao TX do conversor (do leitor)
#define TX_PIN 15  // Só use se precisar enviar dados para o leitor

SoftwareSerial leitorSerial(RX_PIN, TX_PIN);

#define SS_PIN1 26
#define SS_PIN2 33
#define RST_PIN 0
#define BUTTON_PIN 4

#define pinVerde 12
#define pinVermelho 14

#define EEPROM_SIZE 2048
#define MAX_GABARITOS 100    // Número máximo de gabaritos
#define MAX_PECA 10          // Máximo de peças por gabarito
#define UID_SIZE 4
#define MAX_NAME_LENGTH 20

typedef struct {
  byte silkUID[MAX_PECA][UID_SIZE];        // Armazena os UIDs das peças associadas
  char silkNames[MAX_PECA][MAX_NAME_LENGTH]; // Nomes das peças
  uint8_t totalSilks;                        // Quantidade de peças associadas (mínimo 1, máximo 10)
} GabaritoAssociacao;

// Vetor com os UIDs dos gabaritos
byte gabaritosUID[MAX_GABARITOS][UID_SIZE];
// Vetor com as estruturas que associam as peças a cada gabarito
GabaritoAssociacao associacoes[MAX_GABARITOS];

int totalGabaritos = 0;  // Quantos gabaritos foram cadastrados

BluetoothSerial SerialBT;
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 mfrc522_1(SS_PIN1, RST_PIN);
MFRC522 mfrc522_2(SS_PIN2, RST_PIN);

byte masterUID[UID_SIZE];
#define EEPROM_MASTER_ADDR 0
bool masterCadastrado = false;
bool autenticado = false;

volatile bool Cadastrando = false;
volatile bool gabaritolido = false;
volatile bool modoCadastroAtivo = false;
volatile bool interruptFlag = false; // Flag para o botão
bool exibiuMensagemCadastro = false; // Controle para exibir a mensagem de leitura do gabarito
// Função para alternar o modo de operação
unsigned long ultimaAlteracao = 0;       // Variável para armazenar o último tempo de alteração
const unsigned long debounceDelay = 200; // Tempo de debounce em milissegundos

// Declaração de funções
void cadastrarEtiqueta();
void modoOperacao();
bool compararUID(byte *uid1, byte *uid2);
void salvarNaEEPROM();
void carregarDaEEPROM();
void limparEEPROM();
void apagarPar(int indice);
void exibirStatus(String msg1, String msg2);
void exibirStatusBT(String msg1, String msg2);
void salvarCartaoMaster(byte *uid);
void carregarCartaoMaster();
void cadastrarCartaoMaster();
void alternarModo();
String converterUIDParaString(byte *uid, byte tamanho);

// Declarações das tarefas para os núcleos
void taskCadastro(void *pvParameters);
void taskOperacao(void *pvParameters);

// Função de interrupção para alternar modos
void IRAM_ATTR handleButtonPress()
{
    interruptFlag = true; // Sinaliza que o botão foi pressionado
}

// Configuração inicial
void setup()
{
    Serial.begin(9600);
    SerialBT.begin("Silk_Linha_11");
    SPI.begin();
    EEPROM.begin(EEPROM_SIZE);
    lcd.init();
    lcd.backlight();

    // Inicializa a Serial2 nos pinos 15 (RX) e 16 (TX) com baudrate 9600
    Serial2.begin(9600, SERIAL_8N1, 15, 16);

    pinMode(BUTTON_PIN, INPUT_PULLDOWN);
    pinMode(pinVerde, OUTPUT);
    pinMode(pinVermelho, OUTPUT_OPEN_DRAIN);
    // attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, RISING);
    digitalWrite(pinVermelho, LOW);

    mfrc522_1.PCD_Init();
    mfrc522_2.PCD_Init(); // Inicializa o segundo leitor
    carregarCartaoMaster();

    if (!masterCadastrado)
    {
        cadastrarCartaoMaster();
    }
    else
    {
        exibirStatus("Sistema iniciado", "Cartao master OK");
    }

    carregarDaEEPROM();

    // Tarefas em núcleos separados
    xTaskCreatePinnedToCore(
        taskCadastro,
        "TaskCadastro",
        10000,
        NULL,
        1,
        NULL,
        0);

    xTaskCreatePinnedToCore(
        taskOperacao,
        "TaskOperacao",
        10000,
        NULL,
        1,
        NULL,
        1);
}


// Função para alternar o modo de operação
// unsigned long ultimaAlteracao = 0;       // Variável para armazenar o último tempo de alteração
// const unsigned long debounceDelay = 200; // Tempo de debounce em milissegundos
void alternarModo()
{
    unsigned long tempoAtual = millis(); // Obtém o tempo atual

    if (tempoAtual - ultimaAlteracao > debounceDelay) // Verifica se o tempo desde a última alteração é maior que o delay
    {
        modoCadastroAtivo = !modoCadastroAtivo;
        autenticado = false;
        exibiuMensagemCadastro = false;
        if (modoCadastroAtivo)
        {
            exibirStatus("Modo Cadastro", "Aprox. o master");
        }
        else
        {
            exibirStatus("Modo Operacao", "Ativado");
            exibirStatusBT("Modo Operacao", "Ativado");
        }
        ultimaAlteracao = tempoAtual; // Atualiza o tempo da última alteração
    }
}

// Tarefa para o modo de cadastro no Núcleo 0
void taskCadastro(void *pvParameters)
{
    while (1)
    {
        if (modoCadastroAtivo && autenticado)
        {
            cadastrarEtiqueta();
        }
        else if (modoCadastroAtivo && !autenticado)
        {
            if (mfrc522_1.PICC_IsNewCardPresent() && mfrc522_1.PICC_ReadCardSerial())
            {
                if (compararUID(mfrc522_1.uid.uidByte, masterUID))
                {
                    autenticado = true;
                    exibirStatus("Modo Cadastro", "Autenticado");
                    exibirStatusBT("Modo Cadastro", "Cartao master autenticado");
                    delay(2000);
                }
                else
                {
                    exibirStatus("Erro", "Cartao master invalido");
                    exibirStatusBT("Erro", "Cartao master invalido");
                    delay(2000);
                }
                mfrc522_1.PICC_HaltA();
                mfrc522_1.PCD_StopCrypto1();
            }
        }
        vTaskDelay(10);
    }
}

// Tarefa para o modo de operação no Núcleo 1
void taskOperacao(void *pvParameters)
{
    while (1)
    {
        if (!modoCadastroAtivo)
        {
            modoOperacao();
        }
        vTaskDelay(10);
    }
}

void loop()
{
    if (interruptFlag)
    {
        interruptFlag = false;
        alternarModo();
    }
    unsigned long tempoAtual = millis(); // Obtém o tempo atual

    if (tempoAtual - ultimaAlteracao > debounceDelay) // Verifica se o tempo desde a última alteração é maior que o delay
    {
        // Serial.println(analogRead(BUTTON_PIN));
        if (digitalRead(BUTTON_PIN) == LOW && gabaritolido)
        {
            // modoOperacao();
            // digitalWrite(pinVermelho, HIGH);
            gabaritolido = false;
            digitalWrite(pinVermelho, LOW);
            exibirStatus("Modo Operacao", "Leia o gabarito");
            exibirStatusBT("Modo Operacao", "Leia o gabarito");
            // Serial.println("resetando");
        }

        ultimaAlteracao = tempoAtual; // Atualiza o tempo da última alteração
    }

    if (SerialBT.available() && !Cadastrando)
    {
        String comando = SerialBT.readStringUntil('\n');
        comando.trim();

        if (comando == "modo cadastro")
        {
            modoCadastroAtivo = true;
            autenticado = false;
            exibiuMensagemCadastro = false;
            exibirStatus("Modo Cadastro", "Aprox. o master");
            exibirStatusBT("Modo Cadastro", "Aguardando autenticacao");
        }
        else if (comando == "modo producao")
        {
            modoCadastroAtivo = false;
            autenticado = false;
            exibiuMensagemCadastro = false;
            exibirStatus("Modo Operacao", "Ativado via BT");
            exibirStatusBT("Modo Operacao", "Ativado");
        }
        else if (comando.startsWith("apagar"))
        {
            int indice = comando.substring(7).toInt() - 1;
            apagarPar(indice);
        }
        else if (comando == "limpar")
        {
            limparEEPROM();
        }
        else
        {
            SerialBT.println("Comando não reconhecido");
        }
    }
}

bool converterStringParaUID(String str, byte uid[UID_SIZE]) {
  // Verifica se a string tem exatamente 8 caracteres
  if (str.length() != UID_SIZE * 2) {
    return false;
  }
  for (int i = 0; i < UID_SIZE; i++) {
    // Extrai 2 caracteres para cada byte
    String byteStr = str.substring(i * 2, i * 2 + 2);
    uid[i] = (byte) strtol(byteStr.c_str(), NULL, 16);
  }
  return true;
}


void cadastrarEtiqueta()
{
  // Exibe mensagem inicial
  exibirStatus("Modo de cadastro", "Leia o gabarito");
  SerialBT.println("Aproxime o cartão gabarito");

  // Aguarda a leitura do gabarito via NFC
  while (!(mfrc522_1.PICC_IsNewCardPresent() && mfrc522_1.PICC_ReadCardSerial())) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  // Salva o UID do gabarito
  if (totalGabaritos < MAX_GABARITOS) {
    memcpy(gabaritosUID[totalGabaritos], mfrc522_1.uid.uidByte, UID_SIZE);
    mfrc522_1.PICC_HaltA();
    mfrc522_1.PCD_StopCrypto1();

    // Inicializa o contador de peças para este gabarito
    associacoes[totalGabaritos].totalSilks = 0;
    exibirStatus("Gabarito lido", "Cadastre as peças");
    SerialBT.println("Cadastre as peças associadas.");
    SerialBT.println("Aproxime um cartão peça, envie UID via Serial2 ou digite 'fim' para terminar.");

    // Loop para cadastro das peças (máximo 10)
    while (associacoes[totalGabaritos].totalSilks < MAX_PECA) {
      // Se houver comando via BT para finalizar, interrompe o cadastro de peças
      if (SerialBT.available()) {
        String comando = SerialBT.readStringUntil('\n');
        comando.trim();
        if (comando.equalsIgnoreCase("fim")) {
          break;
        }
      }

      // Opção 1: Leitura via sensor NFC
      if (mfrc522_1.PICC_IsNewCardPresent() && mfrc522_1.PICC_ReadCardSerial()) {
        int silkIndex = associacoes[totalGabaritos].totalSilks;
        memcpy(associacoes[totalGabaritos].silkUID[silkIndex], mfrc522_1.uid.uidByte, UID_SIZE);
        mfrc522_1.PICC_HaltA();
        mfrc522_1.PCD_StopCrypto1();

        exibirStatus("Peça lida (NFC)", "Digite o nome via BT");
        SerialBT.println("Peça lida (NFC). Digite o nome da peça:");

        // Aguarda o nome da peça via BT
        while (!SerialBT.available()) {
          vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        String nomeSilk = SerialBT.readStringUntil('\n');
        nomeSilk.trim();
        nomeSilk.toCharArray(associacoes[totalGabaritos].silkNames[silkIndex], MAX_NAME_LENGTH);

        associacoes[totalGabaritos].totalSilks++;
        exibirStatus("Peça cadastrada", nomeSilk);
        SerialBT.println("Peça cadastrada: " + nomeSilk);
        SerialBT.println("Aproxime outra peça, envie UID via Serial2 ou digite 'fim' para terminar.");
        delay(1000); // Pequeno delay para estabilidade
      }
      // Opção 2: Leitura via Serial2 (pinos 15 e 16)
      else if (Serial2.available()) {
        String uidStr = Serial2.readStringUntil('\n');
        uidStr.trim();
        byte tempUID[UID_SIZE];
        if (converterStringParaUID(uidStr, tempUID)) {
          int silkIndex = associacoes[totalGabaritos].totalSilks;
          memcpy(associacoes[totalGabaritos].silkUID[silkIndex], tempUID, UID_SIZE);

          exibirStatus("Peça lida (Serial)", "Digite o nome via BT");
          SerialBT.println("Peça lida via Serial2. Digite o nome da peça:");

          // Aguarda o nome da peça via BT
          while (!SerialBT.available()) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
          }
          String nomeSilk = SerialBT.readStringUntil('\n');
          nomeSilk.trim();
          nomeSilk.toCharArray(associacoes[totalGabaritos].silkNames[silkIndex], MAX_NAME_LENGTH);

          associacoes[totalGabaritos].totalSilks++;
          exibirStatus("Peça cadastrada", nomeSilk);
          SerialBT.println("Peça cadastrada: " + nomeSilk);
          SerialBT.println("Aproxime outra peça, envie UID via Serial2 ou digite 'fim' para terminar.");
          delay(1000);
        }
        else {
          SerialBT.println("Formato UID inválido recebido via Serial2.");
        }
      }
      vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    // Verifica se pelo menos uma peça foi cadastrada
    if (associacoes[totalGabaritos].totalSilks < 1) {
      exibirStatus("Erro", "Minimo 1 peça");
      SerialBT.println("Você deve cadastrar no mínimo 1 peça para cada gabarito!");
      // Aqui você pode optar por descartar o gabarito ou forçar a tentativa novamente
    }
    else {
      totalGabaritos++;
      salvarNaEEPROM(); // Atualize sua função de salvamento para trabalhar com a nova estrutura
      exibirStatus("Cadastro Concluído", "Gabarito cadastrado");
      SerialBT.println("Gabarito cadastrado com sucesso!");
    }
  }
  else {
    exibirStatus("Limite atingido", "");
    SerialBT.println("Número máximo de gabaritos atingido.");
  }
}



void apagarPar(int indice)
{
    // Verifica se o índice está dentro dos limites
    if (indice < 0 || indice >= totalGabaritos)
    {
        exibirStatus("Erro Exclusao", "Indice invalido");
        SerialBT.println("Erro: Indice fora dos limites.");
        return;
    }

    // Desloca os registros a partir do índice para apagar a associação desejada
    for (int i = indice; i < totalGabaritos - 1; i++)
    {
        // Copia o UID do gabarito do próximo registro
        memcpy(gabaritosUID[i], gabaritosUID[i + 1], UID_SIZE);
        // Copia toda a estrutura de associação (que contém as peças e seus nomes)
        associacoes[i] = associacoes[i + 1];
    }

    // Atualiza o total de gabaritos cadastrados
    totalGabaritos--;

    // Salva as alterações na EEPROM
    salvarNaEEPROM();

    exibirStatus("Exclusao", "Associação apagada");
    SerialBT.println("Associação apagada com sucesso!");
}


void limparEEPROM()
{
    for (int i = 0; i < EEPROM_SIZE; i++)
    {
        EEPROM.write(i, 0);
    }
    EEPROM.commit();
    totalGabaritos = 0;
    exibirStatus("EEPROM limpa", "Todos os dados apagados");
    exibirStatusBT("EEPROM limpa", "Todos os dados apagados");
}

void modoOperacao()
{
  static bool listaVaziaExibida = false;
  if (totalGabaritos == 0) {
    if (!listaVaziaExibida) {
      exibirStatus("Lista vazia", "Cadastre um gabarito");
      SerialBT.println("Nenhum gabarito cadastrado. Cadastre um antes.");
      listaVaziaExibida = true;
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    return;
  }
  else {
    listaVaziaExibida = false;
  }

  // Verifica se um cartão de gabarito foi lido
  if (mfrc522_1.PICC_IsNewCardPresent() && mfrc522_1.PICC_ReadCardSerial()) {
    int indiceGabarito = -1;
    for (int i = 0; i < totalGabaritos; i++) {
      if (compararUID(gabaritosUID[i], mfrc522_1.uid.uidByte)) {
        indiceGabarito = i;
        break;
      }
    }
    mfrc522_1.PICC_HaltA();
    mfrc522_1.PCD_StopCrypto1();

    if (indiceGabarito != -1) {
      exibirStatus("Gabarito reconhecido", "Aproxime a peça");
      SerialBT.println("Gabarito reconhecido. Aproxime o cartão da peça.");
      unsigned long inicioEspera = millis();
      bool validPiece = false;
      while (millis() - inicioEspera < 20000) {
        if (mfrc522_2.PICC_IsNewCardPresent() && mfrc522_2.PICC_ReadCardSerial()) {
          // Compara o UID lido com todas as peças associadas a este gabarito
          for (int k = 0; k < associacoes[indiceGabarito].totalSilks; k++) {
            if (compararUID(associacoes[indiceGabarito].silkUID[k], mfrc522_2.uid.uidByte)) {
              String nomeSilk = String(associacoes[indiceGabarito].silkNames[k]);
              exibirStatus("Peça correta", nomeSilk);
              SerialBT.println("Peça correta: " + nomeSilk);
              digitalWrite(pinVermelho, HIGH);
              validPiece = true;
              break;
            }
          }
          mfrc522_2.PICC_HaltA();
          mfrc522_2.PCD_StopCrypto1();
          if (validPiece) break;
          else {
            exibirStatus("Peça incorreta", "Tente novamente");
            SerialBT.println("Peça incorreta. Aproxime novamente o cartão da peça.");
          }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
      }
      if (!validPiece) {
        exibirStatus("Tempo esgotado", "Leia o gabarito novamente");
        SerialBT.println("Tempo esgotado. Por favor, leia o gabarito novamente.");
      }
    }
    else {
      exibirStatus("Erro", "Gabarito não reconhecido");
      SerialBT.println("Gabarito não reconhecido!");
    }
  }
}


void exibirStatus(String msg1, String msg2)
{
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(msg1);
    lcd.setCursor(0, 1);
    lcd.print(msg2);
}

void exibirStatusBT(String msg1, String msg2)
{
    SerialBT.println(msg1);
    if (msg2 != "")
    {
        SerialBT.println(msg2);
    }
}

bool compararUID(byte *uid1, byte *uid2)
{
    for (byte i = 0; i < UID_SIZE; i++)
    {
        if (uid1[i] != uid2[i])
        {
            return false;
        }
    }
    return true;
}

void salvarCartaoMaster(byte *uid)
{
    int addr = EEPROM_MASTER_ADDR;
    for (int i = 0; i < UID_SIZE; i++)
    {
        EEPROM.write(addr++, uid[i]);
    }
    EEPROM.commit();
    masterCadastrado = true;
}

void carregarCartaoMaster()
{
    int addr = EEPROM_MASTER_ADDR;
    for (int i = 0; i < UID_SIZE; i++)
    {
        masterUID[i] = EEPROM.read(addr++);
    }

    bool uidVazio = true;
    for (int i = 0; i < UID_SIZE; i++)
    {
        if (masterUID[i] != 0xFF && masterUID[i] != 0x00)
        {
            uidVazio = false;
            break;
        }
    }

    masterCadastrado = !uidVazio;
}

void cadastrarCartaoMaster()
{
    exibirStatus("Cadastro Master", "Aproxime o cartao");

    while (!mfrc522_1.PICC_IsNewCardPresent() || !mfrc522_1.PICC_ReadCardSerial())
    {
    }

    salvarCartaoMaster(mfrc522_1.uid.uidByte);
    mfrc522_1.PICC_HaltA();
    mfrc522_1.PCD_StopCrypto1();
    exibirStatus("Cartao Master", "Cadastrado");
}

void salvarNaEEPROM()
{
  int addr = EEPROM_MASTER_ADDR + UID_SIZE; // Considerando que o cartão master fica nos primeiros bytes
  EEPROM.write(addr++, totalGabaritos);
  for (int i = 0; i < totalGabaritos; i++) {
    // Salva o UID do gabarito
    for (int j = 0; j < UID_SIZE; j++) {
      EEPROM.write(addr++, gabaritosUID[i][j]);
    }
    // Salva o total de peças para este gabarito
    EEPROM.write(addr++, associacoes[i].totalSilks);
    // Salva cada peça
    for (int k = 0; k < associacoes[i].totalSilks; k++) {
      for (int j = 0; j < UID_SIZE; j++) {
        EEPROM.write(addr++, associacoes[i].silkUID[k][j]);
      }
      for (int j = 0; j < MAX_NAME_LENGTH; j++) {
        EEPROM.write(addr++, associacoes[i].silkNames[k][j]);
      }
    }
  }
  EEPROM.commit();
}


void carregarDaEEPROM()
{
  int addr = EEPROM_MASTER_ADDR + UID_SIZE;
  totalGabaritos = EEPROM.read(addr++);
  for (int i = 0; i < totalGabaritos; i++) {
    // Carrega o UID do gabarito
    for (int j = 0; j < UID_SIZE; j++) {
      gabaritosUID[i][j] = EEPROM.read(addr++);
    }
    // Carrega o total de peças
    associacoes[i].totalSilks = EEPROM.read(addr++);
    // Carrega cada peça
    for (int k = 0; k < associacoes[i].totalSilks; k++) {
      for (int j = 0; j < UID_SIZE; j++) {
        associacoes[i].silkUID[k][j] = EEPROM.read(addr++);
      }
      for (int j = 0; j < MAX_NAME_LENGTH; j++) {
        associacoes[i].silkNames[i][j] = EEPROM.read(addr++);
      }
    }
  }
}


String converterUIDParaString(byte *uid, byte tamanho)
{
    String uidString = "";
    for (byte i = 0; i < tamanho; i++)
    {
        if (uid[i] < 0x10)
            uidString += "0";
        uidString += String(uid[i], HEX);
    }
    uidString.toUpperCase();
    return uidString;
}
