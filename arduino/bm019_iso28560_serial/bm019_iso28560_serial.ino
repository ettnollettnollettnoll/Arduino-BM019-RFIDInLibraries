/*
 * Proof-of-concept for using an Arduino and a BM019 NFC/RFID module from Solutions Cubed
 * for reading from and writing to RF tags for library materials formatted using
 * ISO 28560-3 / DS/INF 28560 "The Danish Data Model"
 * 
 * Intended as a prototype and for inspiration, please
 * DO NOT USE IN PRODUCTION without a serious code review.
 */

/*
 * The code for communication with the BM019 module is based on/copied from examples
 * described in a blog post by Solutions Cubed.
 * 
 * http://blog.solutions-cubed.com/near-field-communication-nfc-with-the-arduino/
*/

/*
 * BM019 wiring from: http://blog.solutions-cubed.com/near-field-communication-nfc-with-the-arduino/
 * 
 * NFC Communication with the Solutions Cubed, LLC BM019
 * and an Arduino Uno.  The BM019 is a module that
 * carries ST Micro's CR95HF, a serial to NFC converter.     
 * 
 * Wiring:
 * Arduino          BM019
 * IRQ: Pin 9       DIN: pin 2
 * SS: pin 10       SS: pin 3
 * MOSI: pin 11     MOSI: pin 5
 * MISO: pin 12     MISO: pin4
 * SCK: pin 13      SCK: pin 6
 * 
 * Refer to the Fritzing sketch for additional wiring.
*/

// The BM019 module communicates using SPI, so include the library
#include <SPI.h>

// Used for communication with the BM019
const int SS_PIN = 10; // slave Select pin
const int IRQ_PIN = 9; // sends wake-up pulse
byte rxBuffer[40]; // receive buffer
byte nfcReady = 0; // used to track NFC state

// The Basic Blocks from ISO 28560-3
int blockContentAndType = 0; // content parameter and type of usage
int blockSet[2] = { 0, 0 }; // the number of tags for an item and the item number (only used for CRC)
int blockPrimaryID[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // barcode or other item ID
int blockCRC[2] = { 0, 0 }; // cyclic redundancy check
int blockISIL[13] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // ISIL or other institution ID
int blockOverflow[2] = { 0, 0 }; // the last bytes of the last memory block

// The maximum length of the barcode/item ID
const int MAX_LENGTH_BARCODE = 16;

// Char version of the barcode/item ID
// C 'strings' end with null or '\0'
char barcode[17] = { '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'};

// The new input barcode/item ID
int newBarcode[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// CRC recorded in the ISO 28560-3 CRC block in the RF tag
char currentReadCrc[5] = { '\0', '\0', '\0', '\0', '\0'};
// CRC calucatad from values in the ISO 28560-3 blocks in the RF tag
char currentCalculatedCrc[5] = { '\0', '\0', '\0', '\0', '\0'};

// Please refer to ISO 28560-1 and your own implementation of the standard for details on how security is handled in your tags.
// Security bits encoded in the ### field
byte currentSecurity = 0;
const byte SECURITY_ON = 1;
const byte SECURITY_OFF = 0;

// LEDs
const byte I_LED_RED = 5; // security off
const byte I_LED_GREEN = 6; // security on
const byte I_LED_YELLOW = 7; // scanning for tags

// Loop the inventory function or not
bool scanForTags = true;

// Command recieved over serial
char externalCommand = '0';

// Commands that can be sent to Arduino
const char COMMAND_READ_TAG = '1';
const char COMMAND_READ_SECURITY = '2';
const char COMMAND_SECURITY_ON = '3';
const char COMMAND_SECURITY_OFF = '4';
const char COMMAND_WRITE_TAG = '5';
const char COMMAND_STOP_SCANNING = '9';

// Used for calculating CRC
const int CRC_POLY = 0x1021;
const int CRC_SUM_INIT = 0xffff;

// Resets most of the global variables
void ResetVariables() {
  int i;
  blockContentAndType = 0;
  blockSet[0] = 0;
  blockSet[1] = 0;
  for (i = 0; i < 16; i++)
    blockPrimaryID[i] = 0;
  blockCRC[0] = 0;
  blockCRC[1] = 0;
  for (i = 0; i < 13; i++)
    blockISIL[i] = 0;
  blockOverflow[0] = 0;
  blockOverflow[1] = 0;
  currentSecurity = 0;
  externalCommand = '0';
  for (i = 0; i < 5; i++)
    currentReadCrc[i] = '\0';
  for (i = 0; i < 5; i++)
    currentCalculatedCrc[i] = '\0';
  for (i = 0; i < 17; i++)
    barcode[i] = '\0';
  for (i = 0; i < 16; i++)
    newBarcode[i] = 0;
}

// Calculating cyclic redundancy check CRC-16-CCITT
// as described in ISO 28560
int UpdateCRC(int c, int crcSum) {
  int i;
  bool xorFlag;
  c <<= 8;
  for (i = 0; i < 8; i++) {
    xorFlag = ((crcSum ^ c) & 0x8000) != 0;
    crcSum = crcSum << 1;
    if (xorFlag)
      crcSum = crcSum ^ CRC_POLY;
    c = c << 1;
  }
  crcSum &= 0xffff;
  return crcSum;
}

/* A reply is always sent after a message i recieved 
 * the reply is emedded using basic JSON with the following keys
 * r: result: 0 for an unsuccessful result, 1 for a successful result
 * e: error: a possible error message
 * s: security: security status, 1 for on and 0 for off or unknown
 * p: payload: payload if applicable
 * c1: CRC recorded: CRC recorded in the RF tag, sent for command 1
 * c2: CRC calculated: CRC calulated from other values in the RF tag, sent for command 1
*/
void SendReply(bool r, const char *e, bool s, const char *p, const char *c1, const char *c2) {
  Serial.print("{");
  // print result 0 or 1
  Serial.print("\"r\":");
  if (r)
    Serial.print("\"1\"");
  else
    Serial.print("\"0\"");
  Serial.print(",");
  // print any error message
  Serial.print("\"e\":");
  Serial.print("\"");
  Serial.print(e);
  Serial.print("\"");
  Serial.print(",");
// print security 0 or 1
  Serial.print("\"s\":");
  if (s)
    Serial.print("\"1\"");
  else
    Serial.print("\"0\"");
  Serial.print(",");
  // print any error message
  Serial.print("\"e\":");
  Serial.print("\"");
  Serial.print(e);
  Serial.print("\"");
  Serial.print(",");
  // print any payload
  Serial.print("\"p\":");
  Serial.print("\"");
  Serial.print(p);
  Serial.print("\"");
  Serial.print(",");
  // print CRC recorded in tag
  Serial.print("\"c1\":");
  Serial.print("\"");
  Serial.print(c1);
  Serial.print("\"");
  Serial.print(",");
  // print calculated CRC
  Serial.print("\"c2\":");
  Serial.print("\"");
  Serial.print(c2);
  Serial.print("\"");
  Serial.print("}");
  Serial.println("");
  Serial.flush(); // waits for the transmission of outgoing serial data to complete
}

/* Set protocol to ISO/IEC 15693
 *  
 *  This requires three steps    
 *  1. send command    
 *  2. poll to see if CR95HF has data    
 *  3. read the response
 *  
*/
void SetProtocol() {
  byte i = 0;

  // step 1 send the command
  digitalWrite(SS_PIN, LOW);
  SPI.transfer(0x00);  // SPI control byte to send command to CR95HF
  SPI.transfer(0x02);  // Set protocol command
  SPI.transfer(0x02);  // length of data to follow
  SPI.transfer(0x01);  // code for ISO/IEC 15693
  SPI.transfer(0x0D);  // Wait for SOF, 10% modulation, append CRC
  digitalWrite(SS_PIN, HIGH);
  delay(1);

  // step 2, poll for data ready
  digitalWrite(SS_PIN, LOW);
  while (rxBuffer[0] != 8) {
    rxBuffer[0] = SPI.transfer(0x03);  // Write 3 until
    rxBuffer[0] = rxBuffer[0] & 0x08;  // bit 3 is set
  }
  digitalWrite(SS_PIN, HIGH);
  delay(1);

  // step 3, read the data
  digitalWrite(SS_PIN, LOW);
  SPI.transfer(0x02);   // SPI control byte for read
  rxBuffer[0] = SPI.transfer(0);  // response code
  rxBuffer[1] = SPI.transfer(0);  // length of data
  digitalWrite(SS_PIN, HIGH);

  if ((rxBuffer[0] == 0) & (rxBuffer[1] == 0)) {
    nfcReady = 1; // NFC is ready
    digitalWrite(I_LED_YELLOW, HIGH);
  } else {
    nfcReady = 0; // NFC not ready
  }
}

// InventoryCommand checks to see if an RF tag is in range of the BM019
bool InventoryCommand() {
  byte i = 0;

  // step 1 send the command
  digitalWrite(SS_PIN, LOW);
  SPI.transfer(0x00);  // SPI control byte to send command to CR95HF
  SPI.transfer(0x04);  // Send Receive CR95HF command
  SPI.transfer(0x02);  // length
  SPI.transfer(0x02);  // request Flags
  SPI.transfer(0x2B);  // Get System Information Command for ISO/IEC 15693
  digitalWrite(SS_PIN, HIGH);
  delay(1);

  // step 2, poll for data ready
  // data is ready when a read byte
  // has bit 3 set (ex:  B'0000 1000')
  digitalWrite(SS_PIN, LOW);
  while (rxBuffer[0] != 8) {
    rxBuffer[0] = SPI.transfer(0x03);  // Write 3 until
    rxBuffer[0] = rxBuffer[0] & 0x08;  // bit 3 is set
  }
  digitalWrite(SS_PIN, HIGH);
  delay(1);

  // step 3, read the data
  digitalWrite(SS_PIN, LOW);
  SPI.transfer(0x02);   // SPI control byte for read
  rxBuffer[0] = SPI.transfer(0);  // response code
  rxBuffer[1] = SPI.transfer(0);  // length of data
  for (i = 0; i < rxBuffer[1]; i++)
    rxBuffer[i + 2] = SPI.transfer(0); // data
  digitalWrite(SS_PIN, HIGH);
  delay(1);

  // Set green or red LED to high to indicate security status
  if (rxBuffer[0] == 128) {
    currentSecurity = rxBuffer[13];
    if (currentSecurity == SECURITY_ON) {
      digitalWrite(I_LED_GREEN, LOW);
      digitalWrite(I_LED_RED, HIGH);
    } else if (currentSecurity == SECURITY_OFF) {
      digitalWrite(I_LED_GREEN, HIGH);
      digitalWrite(I_LED_RED, LOW);
    } else {
      digitalWrite(I_LED_GREEN, LOW);
      digitalWrite(I_LED_RED, LOW);
    }
    return true;
  }
  else {
    // Set LEDs to low to indicate that no tag is in range
    digitalWrite(I_LED_GREEN, LOW);
    digitalWrite(I_LED_RED, LOW);
    return false;
  }
}

// Please refer to ISO 28560-1 and your own implementation of the standard for details on how security is handled in your tags.
// Updates the security bits
bool WriteSecurity(byte newSecurity) {
  /*
   * Dummy function where the code has been withheld.
   */
 
   // Set green or red LED to high to indicate security status
  if (newSecurity == SECURITY_ON) {
    digitalWrite(I_LED_GREEN, LOW);
    digitalWrite(I_LED_RED, HIGH);
  } else if (newSecurity == SECURITY_OFF) {
    digitalWrite(I_LED_GREEN, HIGH);
    digitalWrite(I_LED_RED, LOW);
  }
  return true;
}

// Reads data from a defined number of memory blocks
bool ReadMemory() {
  byte i = 0;

  // step 1 send the command
  digitalWrite(SS_PIN, LOW);
  SPI.transfer(0x00);  // SPI control byte to send command to CR95HF
  SPI.transfer(0x04);  // Send Receive CR95HF command
  SPI.transfer(0x04);  // length of data that follows
  SPI.transfer(0x02);  // request Flags byte
  SPI.transfer(0x23);  // Read Multiple Blocks command for ISO/IEC 15693
  SPI.transfer(0x00);  // First Block Number
  SPI.transfer(0x09);  // Number of Blocks
  digitalWrite(SS_PIN, HIGH);
  delay(1);

  // step 2, poll for data ready
  digitalWrite(SS_PIN, LOW);
  while (rxBuffer[0] != 8)
  {
    rxBuffer[0] = SPI.transfer(0x03);  // Write 3 until
    rxBuffer[0] = rxBuffer[0] & 0x08;  // bit 3 is set
  }
  digitalWrite(SS_PIN, HIGH);
  delay(1);

  // step 3, read the data
  digitalWrite(SS_PIN, LOW);
  SPI.transfer(0x02);   // SPI control byte for read
  rxBuffer[0] = SPI.transfer(0);  // response code
  rxBuffer[1] = SPI.transfer(0);  // length of data
  for (i = 0; i < rxBuffer[1]; i++)
    rxBuffer[i + 2] = SPI.transfer(0); // data
  digitalWrite(SS_PIN, HIGH);
  delay(1);

  if (rxBuffer[0] == 128) { // is response code good?
    // the first three bytes in the rxBuffer are used for response code and length
    blockContentAndType = rxBuffer[3];
    blockSet[0] = rxBuffer[4];
    blockSet[1] = rxBuffer[5];
    for (i = 0; i < 16; i++)
      blockPrimaryID[i] = rxBuffer[6 + i];
    blockCRC[0] = rxBuffer[22];
    blockCRC[1] = rxBuffer[23];
    for (i = 0; i < 13; i++)
      blockISIL[i] = rxBuffer[24 + i];
    blockOverflow[0] = rxBuffer[38];
    blockOverflow[1] = rxBuffer[39];

    // Calculate CRC
    // The result could be used to detect possible read errors but it is not checked at present
    int crcSum = CRC_SUM_INIT;
    crcSum = UpdateCRC(blockContentAndType, crcSum);
    crcSum = UpdateCRC(blockSet[0], crcSum);
    crcSum = UpdateCRC(blockSet[1], crcSum);
    for (i = 0; i < 16; i++)
      crcSum = UpdateCRC(blockPrimaryID[i], crcSum);
    for (i = 0; i < 13; i++)
      crcSum = UpdateCRC(blockISIL[i], crcSum);
    // The first part of the sum is recorded in the second byte and vice versa
    sprintf(currentCalculatedCrc, "%x%x", lowByte(crcSum & 0xff), lowByte((crcSum >> 8) & 0xff));

    // Convert int ID/barcode to char
    for (i = 0; i < 16; i++)
      barcode[i] = (char)blockPrimaryID[i];

    // Convert CRC to char
    sprintf(currentReadCrc, "%x%x", blockCRC[0], blockCRC[1]);
    return true;
  } else {
    return false;
  }
}

// Check if any RF tag is in range
bool FindTagInRange() {
  scanForTags = true;
  digitalWrite(I_LED_YELLOW, HIGH);
  if (nfcReady == 0) {
    SetProtocol(); // ISO 15693 settings
    delay(1000);
    scanForTags = true;
  }
  if (InventoryCommand()) {
    return true;
  } else {
    return false;
  }
}

// Writes data to a block of memory.
bool WriteMemory(int *Data, byte Memory_Block, byte Number_of_Bytes) {
  
  byte i = 0;
  // step 1 send the command
  digitalWrite(SS_PIN, LOW);
  SPI.transfer(0x00);  // SPI control byte to send command to CR95HF
  SPI.transfer(0x04);  // Send Receive CR95HF command
  SPI.transfer(0x03 + Number_of_Bytes);  // length of data that follows
  SPI.transfer(0x02);  // request Flags byte
  SPI.transfer(0x21);  // Write Single Block command for ISO/IEC 15693
  SPI.transfer(Memory_Block);  // Memory block address
  for (i = 0; i < Number_of_Bytes; i++)
    SPI.transfer(Data[i]);
  digitalWrite(SS_PIN, HIGH);
  delay(1);

  // step 2, poll for data ready
  // data is ready when a read byte
  // has bit 3 set (ex:  B'0000 1000')
  digitalWrite(SS_PIN, LOW);
  while (rxBuffer[0] != 8) {
    rxBuffer[0] = SPI.transfer(0x03);  // Write 3 until
    rxBuffer[0] = rxBuffer[0] & 0x08;  // bit 3 is set
  }
  digitalWrite(SS_PIN, HIGH);
  delay(1);

  // step 3, read the data
  digitalWrite(SS_PIN, LOW);
  SPI.transfer(0x02);   // SPI control byte for read
  rxBuffer[0] = SPI.transfer(0);  // response code
  rxBuffer[1] = SPI.transfer(0);  // length of data
  for (i = 0; i < rxBuffer[1]; i++)
    rxBuffer[i + 2] = SPI.transfer(0); // data
  digitalWrite(SS_PIN, HIGH);
  delay(1);

  if (rxBuffer[0] == 128){ // is response code good?
    return true;
  } else {
    return false;
  }
}

// Read from the input buffer until it is empty
void forceSerialInputFlush() {
  while (Serial.available() > 0) {
    char t = Serial.read();
  }
}

// Standard Arduino setup function
void setup() {
  pinMode(I_LED_RED, OUTPUT);
  pinMode(I_LED_GREEN, OUTPUT);
  pinMode(I_LED_YELLOW, OUTPUT);

  digitalWrite(I_LED_RED, LOW);
  digitalWrite(I_LED_GREEN, LOW);
  digitalWrite(I_LED_YELLOW, LOW);

  pinMode(IRQ_PIN, OUTPUT);
  digitalWrite(IRQ_PIN, HIGH); // Wake up pulse
  pinMode(SS_PIN, OUTPUT);
  digitalWrite(SS_PIN, HIGH);

  Serial.begin(9600);
  SPI.begin();
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);
  SPI.setClockDivider(SPI_CLOCK_DIV32);

  // The CR95HF requires a wakeup pulse on its IRQ_IN pin
  // before it will select UART or SPI mode.  The IRQ_IN pin
  // is also the UART RX pin for DIN on the BM019 board.
  delay(10); // send a wake up pulse to put the BM019 into SPI mode
  digitalWrite(IRQ_PIN, LOW);
  delayMicroseconds(100);
  digitalWrite(IRQ_PIN, HIGH);
  delay(10);
}

// Standard Arduino loop function
void loop() {
  int i = 0;
  bool sec = false;

  while (!Serial.available() && scanForTags) {
    if (nfcReady == 0) {
      SetProtocol(); // ISO 15693 settings
      delay(1000);
    }
    InventoryCommand();
    delay(100);
  }

  // Read command from serial
  String externalCommandString = Serial.readString();
  Serial.flush();
  char externalCommandChar[externalCommandString.length() + 1];
  externalCommandString.toCharArray(externalCommandChar, externalCommandString.length() + 1);
  externalCommand = externalCommandChar[0];

  if (externalCommand == COMMAND_READ_TAG) {
    if (FindTagInRange()) {
      if (ReadMemory()) {
        sec = currentSecurity == SECURITY_ON;
        SendReply(true, "", sec, barcode, currentReadCrc, currentCalculatedCrc);
      } else {
        SendReply(false, "Failed to read Memory Block", false, "", "", "");
      }
    } else {
      SendReply(false, "No TAG in range", false, "", "", "");
    }
    ResetVariables();
  }
  
  else if (externalCommand == COMMAND_READ_SECURITY) {
    if (FindTagInRange()) {
      if (currentSecurity == SECURITY_ON)
        SendReply(true, "", true, "1", "", "");
      else if (currentSecurity == SECURITY_OFF)
        SendReply(true, "", false, "0", "", "");
      else
        SendReply(false, "Unknown security status", false, "", "", "");
    } else {
      SendReply(false, "No TAG in range", false, "", "", "");
    }
    ResetVariables();
  }

  else if (externalCommand == COMMAND_SECURITY_ON) {
    if (FindTagInRange()) {
      if (WriteSecurity(SECURITY_ON))
        SendReply(true, "", true, "", "", "");
      else
        SendReply(false, "Write Security Command FAIL", false, "", "", "");
    } else {
      SendReply(false, "No TAG in range", false, "", "", "");
    }
    ResetVariables();
  }

  else if (externalCommand == COMMAND_SECURITY_OFF) {
    if (FindTagInRange()) {
      if (WriteSecurity(SECURITY_OFF))
        SendReply(true, "", false, "", "", "");
      else
        SendReply(false, "Write Security Command FAIL", false, "", "", "");
    } else {
      SendReply(false, "No TAG in range", false, "", "", "");
    }
    ResetVariables();
  }

  else if (externalCommand == COMMAND_WRITE_TAG) {
    if (FindTagInRange()) {
      if (ReadMemory()) {
        sec = currentSecurity == SECURITY_ON;
        // To get the barcode remove the first char in the command
        // but C "strings" are terminated by an extra null char
        if (MAX_LENGTH_BARCODE + 1 >= ((sizeof(externalCommandChar) / sizeof(externalCommandChar[0])) - 1)) {
          char barcodeInput[(sizeof(externalCommandChar) / sizeof(externalCommandChar[0])) - 1];
          for (i = 0; i < (sizeof(externalCommandChar) / sizeof(externalCommandChar[0])) - 1; i++) {
            barcodeInput[i] = externalCommandChar[i + 1];
          }
          // Update newBarcode, max size is 16, the remaining bytes at the end of newBarcode will still be 0
          for (i = 0; i < (sizeof(barcodeInput) / sizeof(barcodeInput[0])); i++) {
            newBarcode[i] = barcodeInput[i] - 0; // -0 converts char to int
          }
          // Calculate new CRC
          int crcSum = CRC_SUM_INIT;
          crcSum = UpdateCRC(blockContentAndType, crcSum);
          crcSum = UpdateCRC(blockSet[0], crcSum);
          crcSum = UpdateCRC(blockSet[1], crcSum);
          for (i = 0; i < 16; i++) {
            crcSum = UpdateCRC(newBarcode[i], crcSum);
          }
          for (i = 0; i < 13; i++) {
            crcSum = UpdateCRC(blockISIL[i], crcSum);
          }
          // The first part of the sum is recorded in the second byte and vice versa
          byte calcCRCPart1 = lowByte(crcSum & 0xff);
          byte calcCRCPart2 = lowByte((crcSum >> 8) & 0xff);
          // The blocks that will be written to the RF tag
          int block0[4] = {blockContentAndType, blockSet[0], blockSet[1], newBarcode[0]};
          int block1[4] = {newBarcode[1], newBarcode[2], newBarcode[3], newBarcode[4]};
          int block2[4] = {newBarcode[5], newBarcode[6], newBarcode[7], newBarcode[8]};
          int block3[4] = {newBarcode[9], newBarcode[10], newBarcode[11], newBarcode[12]};
          int block4[4] = {newBarcode[13], newBarcode[14], newBarcode[15], (int)calcCRCPart1};
          int block5[4] = {(int)calcCRCPart2, blockISIL[0], blockISIL[1], blockISIL[2]};
          int block6[4] = {blockISIL[3], blockISIL[4], blockISIL[5], blockISIL[6]};
          int block7[4] = {blockISIL[7], blockISIL[8], blockISIL[9], blockISIL[10]};
          int block8[4] = {blockISIL[11], blockISIL[12], blockOverflow[0], blockOverflow[1]};
          // Write the blocks
          bool writeOK = true;
          if (!WriteMemory(block0, 0, 4)) {
            writeOK = false;
          }
          if (!WriteMemory(block1, 1, 4)) {
            writeOK = false;
          }
          if (!WriteMemory(block2, 2, 4)) {
            writeOK = false;
          }
          if (!WriteMemory(block3, 3, 4)) {
            writeOK = false;
          }
          if (!WriteMemory(block4, 4, 4)) {
            writeOK = false;
          }
          if (!WriteMemory(block5, 5, 4)) {
            writeOK = false;
          }
          if (!WriteMemory(block6, 6, 4)) {
            writeOK = false;
          }
          if (!WriteMemory(block7, 7, 4)) {
            writeOK = false;
          }
          if (!WriteMemory(block8, 8, 4)) {
            writeOK = false;
          }
          if (writeOK) {
            SendReply(true, "", sec, barcodeInput, "", "");
          } else {
            SendReply(false, "Failed to write Memory Block", sec, "", "", "");
          }
        } else {
          SendReply(false, "Barcode to long", sec, "", "", "");
        }
      } else {
        SendReply(false, "Failed to read Memory Block", false, "", "", "");
      }
    } else {
      SendReply(false, "No TAG in range", false, "", "", "");
    }
    ResetVariables();
  }
  
  else if (externalCommand == COMMAND_STOP_SCANNING) {
    scanForTags = false;
    digitalWrite(I_LED_YELLOW, LOW);
    digitalWrite(I_LED_GREEN, LOW);
    digitalWrite(I_LED_RED, LOW);
    SendReply(true, "", false, "", "", "");
  }
  
  else {
    SendReply(false, "Invalid command", false, externalCommandChar, "", "");
    forceSerialInputFlush();
  }
}

