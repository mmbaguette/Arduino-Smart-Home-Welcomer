#include <ESP8266WiFi.h>
#include <ESPAsyncUDP.h>
#include <Pinger.h>
#include <SoftwareSerial.h>
#include <LiquidCrystal.h>
#include <ESP_EEPROM.h>
#include <mdns.h>
#include <ESP_Mail_Client.h>

#define SMTP_HOST "smtp.office365.com"
#define SMTP_PORT 587


/*
 * TODO:
 * Do not acknowledge DHCP lease release (device left network) as a join
 * Send email (attach link to front door cam, maybe one that goes to that point in time?)
 * Attach list of IPs and Device # to email
 */

extern "C"
{
#include <lwip/icmp.h> // needed for icmp packet definitions
#include <lwip/ip_addr.h>
}

#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MACSTR "%02X:%02X:%02X:%02X:%02X:%02X"
#define IP2STR(a) (a)[0], (a)[1], (a)[2], (a)[3]
#define IPSTR "%d.%d.%d.%d"
#define COMMS_TX D0                                                                       // trasmit (not in use)
#define COMMS_RX D1                                                                       // receive
#define SHOW_NEW_TEXT !(millis() - lastLCDText > LCDTextInterval) || LCDTextInterval == 0 // 0 for forever, or time's up; (are we showing the newly set LCD text or the normal one?)
#define MAX_LCD 16                                                                        // max LCD chars on a line
#define MAX_CLIENTS 10
#define IP_ADDR_STR 16 // max length of a given IP address as string

char normalTop[] = "Listening on..."; // default LCD text
char normalBot[MAX_LCD];              // will be assigned when device IP is given

const int RS = 4, EN = 0, d4 = 12, d5 = 13, d6 = 15, d7 = 3; // pins for lcd
LiquidCrystal lcd(RS, EN, d4, d5, d6, d7);                   // initializing C++ classes to use imported libraries
SoftwareSerial Uno(COMMS_RX, COMMS_TX);
Pinger pinger;
AsyncUDP udp;
SMTPSession smtp;
ESP_Mail_Session session;
IPAddress broadcastIP(255, 255, 255, 255); // When an mDNS packet gets parsed this optional callback gets called.

unsigned int recvUdpPort = 67; // DHCP port (unsigned for positive)
char recvPkt[301];             // char array buffer for packet contents

uint8_t macs[MAX_CLIENTS][6];
uint8_t ips[MAX_CLIENTS][4];
char mdnsNames[MAX_CLIENTS][MAX_LCD]; // list of Apple Bonjour names; same index as in ips and macs

const char submitChar = '#'; // keypad char used to submit IP address
bool addingIP = false;
bool removingIP = false;

uint8_t currentMac[6];
uint8_t currentIP[4];
uint8_t joinedIP[4];
uint8_t joinedMac[6];

unsigned long lastLCDText = 0;
unsigned long LCDTextInterval = 3000;
char LCDTextTop[MAX_LCD];     // top LCD text
char LCDTextBot[MAX_LCD];     // bottom LCD text
char LCDTextDoneTop[MAX_LCD]; // what to show when text is done
char LCDTextDoneBot[MAX_LCD];
bool showingLCDText = false;

IPAddress givenIP;
char StrIP[IP_ADDR_STR];
bool pinging = false;
int charsGiven = 0;
int ipsMacsIndex;

boolean newUserAlert = false;
int newUserIndex;
unsigned long lastEmailJoinAlert = 0;
unsigned long emailJoinAlertInterval = 5 * 0 + 6 * 1000; // 5 minutes, 6 seconds

void smtpCallback(SMTP_Status status); // callback when email sent
void answerCallback(const mdns::Answer *answer);
mdns::MDns my_mdns(NULL, NULL, answerCallback); // register callback

void setup()
{
  Serial.begin(115200);
  Uno.begin(4800);     // communication with UNO
  lcd.begin(16, 2);    // setup columns and rows
  lcd.setCursor(0, 0); // top left
  Serial.print("\nConnecting to Wi-Fi");
  lcd.print("Connecting");
  lcd.setCursor(0, 1); // set writing cursor to first column (0) and bottom row (1)

  WiFi.mode(WIFI_STA);              // set wifi mode to station to connect
  WiFi.begin("kmsmk2", "57GtX/18"); // connect to SSID with PSK

  int i; // loop number
  while (WiFi.status() != WL_CONNECTED)
  { // while not connected
    i++;
    delay(500);
    Serial.print(".");
    lcd.printf("%.*s", i, "."); // display i number of dots
  }

  strcpy(normalBot, WiFi.localIP().toString().c_str());
  Serial.printf("\nConnected! IP address: " IPSTR ".\n", IP2STR(WiFi.localIP()));

  my_mdns.begin();
  Serial.println("Started mDNS service.");

  smtp.debug(0);
  smtp.callback(smtpCallback);
  session.server.host_name = SMTP_HOST;
  session.server.port = SMTP_PORT;
  session.login.email = AUTHOR_EMAIL;
  session.login.password = AUTHOR_PASSWORD;
  session.login.user_domain = "";
  Serial.printf("Logged in to email %s.\n", AUTHOR_EMAIL);

  EEPROM.begin(sizeof(int) + MAX_CLIENTS * sizeof(uint8_t) * 10); // size of eeprom basically
  int listLength = 0;                                             // default value if no listLength was set yet in EEPROM
  EEPROM.get(0, listLength);

  if (listLength != 0 && listLength <= MAX_CLIENTS)
  {
    Serial.printf("Loading %d saved client%s!\n", listLength, listLength > 1 ? "s" : "");

    for (int i = 0; i < listLength; i++)
    { // copy each set of mac and ip addresses
      Serial.print("MAC: ");

      for (int j = 0; j < 6; j++)
      {                                                                                       // copy mac address (array of 6 uint8)
        EEPROM.get(sizeof(int) + i * sizeof(uint8_t) * 10 + j * sizeof(uint8_t), macs[i][j]); // 10 uint8's: 6 for macs, 4 for ips
        Serial.printf("%02X:", macs[i][j]);
      }
      Serial.println();
      Serial.print("IP: ");

      for (int k = 0; k < 4; k++)
      { // copy ip address (array of 4 uint8)
        EEPROM.get(sizeof(int) + i * sizeof(uint8_t) * 10 + sizeof(uint8_t) * 6 + k * sizeof(uint8_t), ips[i][k]);
        Serial.printf("%d.", ips[i][k]);
      }
      Serial.println("\n------------");
    }
  }
  else
    Serial.println("No clients saved!");

  if (udp.listen(broadcastIP, recvUdpPort))
  {
    Serial.printf("Listening on UDP port %d and broadcast IP 255.255.255.255 for DHCP requests.\n", recvUdpPort);
    newLCDText(normalTop, normalTop, 0, 0);
    newLCDText(normalBot, normalBot, 0, 1);

    udp.onPacket([](AsyncUDPPacket packet)
                 {
      if (packet.length() >= 33) {
        memcpy(joinedMac, packet.data() + 28, 6);
        memcpy(joinedMac, packet.data() + 28, 6);
        int macIndex = searchMAC(joinedMac); // is this MAC address part of our list? where?
        Serial.printf(MACSTR " joined the network!\n", MAC2STR(joinedMac));

        if (macIndex > -1) { //mac already exists
          char deviceName[MAX_LCD+1];
          memcpy(joinedIP, &ips[macIndex], 4);

          if (mdnsNames[macIndex][0] == 0) {  //hostname was found
            sprintf(deviceName, "Device %d", macIndex + 1); //default device name
            strncpy(&mdnsNames[macIndex][0], deviceName, MAX_LCD); //store this default hostname name for now
          }

          strncpy(deviceName, mdnsNames[macIndex], MAX_LCD); //if anything changed, deviceName will become the real hostname
          deviceName[MAX_LCD] = '\0'; // indicate end of string
          
          if (millis() >= lastEmailJoinAlert + emailJoinAlertInterval) { //don't spam email
            newUserAlert = true;
            newUserIndex = macIndex;
            lastEmailJoinAlert = millis();
          } else {
            Serial.printf("Difference: %d. Interval: %d\n", millis() - lastEmailJoinAlert, emailJoinAlertInterval);
          }
      
          newLCDText("Welcome,", normalTop, 5000, 0);
          newLCDText(deviceName, normalBot, 5000, 1); //why does this also show its IP??
        }
    } });
  }

  // if ping received, and contains MAC, then stop pinging since we're done
  pinger.OnReceive([](const PingerResponse &response)
                   {
    if (response.ReceivedResponse && response.DestMacAddress->addr) {
      Serial.printf( //display ping statistics
        "Reply from %s: bytes=%d time=%lums TTL=%d\n",
        response.DestIPAddress.toString().c_str(),
        response.EchoMessageSize - sizeof(struct icmp_echo_hdr),
        response.ResponseTime,
        response.TimeToLive);

      memcpy(currentMac, response.DestMacAddress->addr, 6);
      memcpy(currentIP, response.DestIPAddress, 4);

      if (calcClients() < MAX_CLIENTS) { // as long as the clients index is smaller than the max number of clients
        int clientIndex;
        int clientMacIndex = searchMAC(currentMac); //use index of mac that already exists, or -1
        int clientIPIndex = searchIP(currentIP); //use index of ip that already exists, or -1

        if (clientMacIndex == -1 && clientIPIndex == -1) // not duplicate IP or MAC
          clientIndex = calcClients(); // make it the next client index on the list.
        else if (clientMacIndex > -1) {
          clientIndex = clientMacIndex;
          Serial.printf(MACSTR " is a duplicate MAC address, as Device %d\n", MAC2STR(currentMac), clientMacIndex + 1);
          return false;
        } else if (clientIPIndex > -1) {
          clientIndex = clientIPIndex;
          Serial.printf(IPSTR " is a duplicate IP address, as Device %d\n", IP2STR(currentIP), clientIPIndex + 1);
          return false;
        }

        memcpy(&macs[calcClients()], currentMac, 6);
        memcpy(&ips[calcClients()], currentIP, 4);
        saveIPsMACs();
        
        char deviceAddedMsg[MAX_LCD];
        sprintf(deviceAddedMsg, "Added Device %d:", clientIndex); 
        newLCDText(deviceAddedMsg, normalTop, 3000, 0);
        newLCDText(givenIP.toString().c_str(), normalBot, 3000, 1);
        Serial.printf("\nAdded " MACSTR " (Device %d) \n", MAC2STR(currentMac),  calcClients());
      } else {
        Serial.printf("Cannot add " MACSTR " . Reached the maximum number of clients (%d).\n", MAC2STR(currentMac), MAX_CLIENTS);
        newLCDText("Can't add IP:", normalTop, 3000, 0);
        newLCDText(givenIP.toString().c_str(), normalBot, 3000, 1);
      }
      return false; //stop pinging
    } else {
      Serial.printf("Request timed out.\n");
      return true; //keep trying to ping
    } });

  pinger.OnEnd([](const PingerResponse &response)
               {
    if (response.TotalReceivedResponses == 0 || !response.DestMacAddress->addr) { //no response ):
      newLCDText("Can't add IP:", normalTop, 3000, 0);
      newLCDText(givenIP.toString().c_str(), normalBot, 3000, 1);
    }
    pinging = false;
    return true; });

  // empty Uno serial buffer
  while (Uno.available() > 0)
    Uno.read();
}

void loop()
{
  delay(30);
  char givenIPChar;
  my_mdns.loop(); // will allow mdns library to work continuously. must be frequently called
  showLCDText();

  if (newUserAlert)
  { // if we need alert and we haven't already
    char deviceName[MAX_LCD];
    char IP_str[MAX_LCD];
    strcpy(deviceName, &mdnsNames[newUserIndex][0]);
    sprintf(IP_str, IPSTR, IP2STR(ips[newUserIndex]));

    Serial.printf("%s (%s) has arrived!\n", deviceName, IP_str);
    send_alert(deviceName, IP_str);
    newUserAlert = false; // no more alerts to send now
  }

  if (pinging)
    return; // everything you want to happen after pinging is done goes below

  if (Uno.available() > 0)
    givenIPChar = Uno.read(); // will get char sent from Arduino serial instead of user's serial monitor
  else
    return; // nothing received from keypad

  // process keypad presses
  switch (givenIPChar)
  {
  case 'A': // user pressed A to begin entering an IP (and they're not typing already)
    if (!addingIP && !pinging && !removingIP)
    {
      char EnterIPHolder[MAX_LCD]; // Enter an IP (submitChar):
      sprintf(EnterIPHolder, "Enter an IP (%c):", submitChar);
      newLCDText(EnterIPHolder, normalTop, 0, 0);
      newLCDText("_", normalBot, 0, 1); // start with empty space

      resetInput();    // reset string buffers that might have previously contained strings from the last time the user tried to enter an IP
      addingIP = true; // allow rest of program to know that user is entering an IP
      return;
    }
    break;
  case 'B': // backspace
    if ((addingIP || removingIP) && charsGiven > 0)
    { // delete last char
      StrIP[charsGiven - 1] = '\0';
      charsGiven--;
      updateIPDisplay();
    }
    break;
  case 'D': // delete IP or device #
    if (!addingIP && !pinging && !removingIP)
    {
      Serial.println("IP deleting prompt.");
      char EnterIPHolder[] = "Del IP or Device";
      newLCDText(EnterIPHolder, normalTop, 0, 0);
      newLCDText("_", normalBot, 0, 1); // start with empty space

      resetInput();      // reset string buffers that might have previously contained strings from the last time the user tried to enter an IP
      removingIP = true; // allow rest of program to know that user is entering an IP
      return;
    }
    break;
  case submitChar: // submitting IP to be added
    Serial.printf("You entered: %s\n", StrIP);

    if (addingIP)
    {
      if (givenIP.fromString(StrIP))
      { // valid IP!
        find_ip();
      }
      else
      { // invalid IP format
        Serial.println("Invalid IP address!");

        char invalidIPStr[MAX_LCD];
        sprintf(invalidIPStr, "%-16s", StrIP);
        newLCDText("Wrong IP format!", normalTop, 3000, 0);
        newLCDText(invalidIPStr, normalBot, 3000, 1);
      }
    }
    else if (removingIP)
    {
      int givenDeviceNum = atoi(StrIP);

      if (givenIP.fromString(StrIP))
      { // valid IP!
        uint8_t ip2Remove[4] = {givenIP[0], givenIP[1], givenIP[2], givenIP[3]};
        int foundIPIndex = searchIP(ip2Remove);

        if (foundIPIndex > -1) // IP exists, let's remove it
          remove_element_dev(foundIPIndex);
        else
        {
          newLCDText("No existing IP:", normalTop, 3000, 0);
          newLCDText(StrIP, normalBot, 3000, 1);
        }
      }
      else if (givenDeviceNum != 0)
      { // they gave device number instead of IP
        Serial.printf("got device number %d\n", givenDeviceNum);
        if (givenDeviceNum > 0 && givenDeviceNum <= MAX_CLIENTS && macs[givenDeviceNum - 1][0] != NULL)
        { // the device number exists
          remove_element_dev(givenDeviceNum - 1);
        }
        else
        { // device number does not exist
          newLCDText("No IP or Device:", normalTop, 3000, 0);
          newLCDText(StrIP, normalBot, 3000, 1);
        }
      }
      else
      {
        newLCDText("No IP or Device:", normalTop, 3000, 0);
        newLCDText(StrIP, normalBot, 3000, 1);
      }
    }
    resetInput();
    break;
  default:
    if (charsGiven < IP_ADDR_STR && (addingIP || removingIP) && !isalpha1(givenIPChar))
    {
      StrIP[charsGiven] = givenIPChar == '*' ? '.' : givenIPChar; // replace * with .
      charsGiven++;
      updateIPDisplay();
    }
    break;
  }
}

int calcClients()
{ // calculates the number of clients saved
  int clients = 0;

  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    if (ips[i][0] != NULL) // if first byte in IP address is not 0
      clients++;
  }
  return clients;
}

int searchIP(uint8_t IP2Search[4])
{
  int foundIPIndex = -1; // index of matched IP from ips, that appeared in an mdns broadcast with a name

  for (int ip = 0; ip < MAX_CLIENTS; ip++)
  { // check if 4-uint8 array is same to string split into 4
    if (foundIPIndex < 0)
    { // haven't found that IP yet
      for (int i = 0; i < 4; i++)
      {
        if (IP2Search[i] == ips[ip][i]) // if octet in IP2Search is the same as
          foundIPIndex = ip;            // the octet in the ip being searched from ips
        else
        {
          foundIPIndex = -1; // reset foundIPIndex, this is not the one.
          break;
        }
      }
    }
  }
  return foundIPIndex;
}

int searchMAC(uint8_t mac2Search[6])
{
  int macIndex = -1; // can we find a matching mac address?

  for (int macI = 0; macI < MAX_CLIENTS; macI++)
  {
    if (macIndex < 0)
    { // haven't found one yey
      for (int hexI = 0; hexI < 6; hexI++)
      {
        if (macs[macI][hexI] == mac2Search[hexI])
          macIndex = macI;
        else
        {
          macIndex = -1;
          break;
        }
      }
    }
  }
  return macIndex;
}

void find_ip()
{
  if (!pinger.Ping(givenIP))
  {
    Serial.println("Error during ping command.");
  }
  else
  {
    pinging = true;
    newLCDText("Searching IP:", normalTop, 0, 0);
    newLCDText(givenIP.toString().c_str(), normalBot, 0, 1);
    Serial.println("Pinging this address.");
  }
}

// answer->rdata_buffer contains IP string (char array)
// answer->name_buffer contains hostname
void answerCallback(const mdns::Answer *answer)
{
  if (answer->rrtype == MDNS_TYPE_A)
  {
    IPAddress searchedIP;                        // initialize IPAddress
    searchedIP.fromString(answer->rdata_buffer); // parse IP data
    uint8_t searchedIPArr[4] = {searchedIP[0], searchedIP[1], searchedIP[2], searchedIP[3]};
    int foundIPIndex = searchIP(searchedIPArr);

    if (foundIPIndex > -1)
    { // this IP was added, so let's add its name to the list!
      char mdnsName[MAX_LCD];
      strncpy(mdnsName, answer->name_buffer, MAX_LCD); // store hostname in mdnsName
      char *lastDot = strrchr(mdnsName, '.');          // return pointer to last occurence of period in string

      if (lastDot) // does the dot exist
        mdnsName[lastDot - mdnsName] = '\0';
      strncpy(&mdnsNames[foundIPIndex][0], mdnsName, MAX_LCD); // store/update name in the same index as the IP is stored in ips array
    }
  }
}

void send_alert(const char *name, const char *IP_str)
{
  char subject[30];
  sprintf(subject, "%s has arrived!", name);
  SMTP_Message message;
  message.sender.name = "Unhackable Smart Homes\u00AE";
  message.sender.email = AUTHOR_EMAIL;
  message.subject = subject;
  message.addRecipient("Home", RECIPIENT_EMAIL);
  Serial.printf("Sending email \"%s\" to %s.\n", subject, RECIPIENT_EMAIL);

  std::string htmlMsg = std::string("<h1>") + name + " joined the network!</h1><br><p>Device " + name + " (" + IP_str + ")</p>";
  message.html.content = htmlMsg;
  message.text.charSet = "us-ascii";
  message.html.transfer_encoding = Content_Transfer_Encoding::enc_7bit;

  if (!smtp.connect(&session)) // connect
    return;                    // return if not connected

  if (!MailClient.sendMail(&smtp, &message))
    Serial.println("Error sending Email, " + smtp.errorReason());
}

void smtpCallback(SMTP_Status status)
{
  Serial.println(status.info());

  if (status.success())
  {
    Serial.println("----------------");
    ESP_MAIL_PRINTF("Message sent success: %d\n", status.completedCount());
    ESP_MAIL_PRINTF("Message sent failled: %d\n", status.failedCount());
    Serial.println("----------------\n");
    struct tm dt;

    for (size_t i = 0; i < smtp.sendingResult.size(); i++)
    {
      SMTP_Result result = smtp.sendingResult.getItem(i);
      time_t ts = (time_t)result.timestamp;
      localtime_r(&ts, &dt);

      ESP_MAIL_PRINTF("Message No: %d\n", i + 1);
      ESP_MAIL_PRINTF("Status: %s\n", result.completed ? "success" : "failed");
      ESP_MAIL_PRINTF("Date/Time: %d/%d/%d %d:%d:%d\n", dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday, dt.tm_hour, dt.tm_min, dt.tm_sec);
      ESP_MAIL_PRINTF("Recipient: %s\n", result.recipients);
      ESP_MAIL_PRINTF("Subject: %s\n", result.subject);
    }
    Serial.println("----------------\n");
  }
}

bool isalpha1(unsigned char ch)
{ // is this character a letter?
  return unsigned((ch & (~(1 << 5))) - 'A') <= 'Z' - 'A';
}

void remove_element_dev(int index)
{ // remove device
  for (int k = index; k < MAX_CLIENTS - 1; k++)
  {
    for (int j = 0; j < 4; j++)
      ips[k][j] = ips[k + 1][j];
  }
  for (int k = index; k < MAX_CLIENTS - 1; k++)
  {
    for (int j = 0; j < 6; j++)
      macs[k][j] = macs[k + 1][j];
  }
  for (int k = index; k < MAX_CLIENTS - 1; k++)
  {
    for (int j = 0; j < MAX_LCD; j++)
      mdnsNames[k][j] = mdnsNames[k + 1][j];
  }
  newLCDText("Removed device:", normalTop, 3000, 0);
  newLCDText(StrIP, normalBot, 3000, 1);
  Serial.printf("Removed device %s at index %d from macs, ips and mdnsNames.\n", StrIP, index);
  saveIPsMACs();
}

bool saveIPsMACs()
{
  for (int i = 0; i < calcClients(); i++)
  {
    for (int j = 0; j < 6; j++)                                                             // save mac address (array of 6 uint8)
      EEPROM.put(sizeof(int) + i * sizeof(uint8_t) * 10 + j * sizeof(uint8_t), macs[i][j]); // 10 uint8's: 6 for macs, 4 for ips
    for (int k = 0; k < 4; k++)                                                             // save ip address (array of 4 uint8)
      EEPROM.put(sizeof(int) + i * sizeof(uint8_t) * 10 + sizeof(uint8_t) * 6 + k * sizeof(uint8_t), ips[i][k]);
  }

  EEPROM.put(0, calcClients()); // set list size
  bool committed = EEPROM.commitReset();
  Serial.println((committed) ? "Commit OK" : "Commit failed");
  return committed;
}

void resetInput()
{
  charsGiven = 0;
  memset(&StrIP[0], 0, sizeof(StrIP));
  addingIP = false;   // not adding an IP anymore since they just submitted one
  removingIP = false; // not removing IP anymore
}

void updateIPDisplay()
{
  char IPInput[MAX_LCD];
  sprintf(IPInput, "%s_", StrIP);       // given IP, then underscore
  newLCDText(IPInput, normalBot, 0, 1); // update display with new input
}

void newLCDText(const char *text, const char *textDone, int interval, int row)
{ // will begin showing text
  LCDTextInterval = interval;
  lastLCDText = millis();

  if (row == 0)
  { // row is y, columns are x
    memcpy(LCDTextTop, text, MAX_LCD);
    memcpy(LCDTextDoneTop, textDone, MAX_LCD);
  }
  else if (row == 1)
  { // bottom LCD text
    memcpy(LCDTextBot, text, MAX_LCD);
    memcpy(LCDTextDoneBot, textDone, MAX_LCD);
  }
}

void showLCDText()
{ // will display text once from global LCDText. will set showingLCDText if time's up.
  lcd.setCursor(0, 0);

  if (!LCDTextTop[0]) // empty string
    lcd.print("");
  else
    lcd.printf("%-16s", SHOW_NEW_TEXT ? LCDTextTop : LCDTextDoneTop);

  lcd.setCursor(0, 1);

  if (!LCDTextBot[0])
    lcd.print("");
  else
    lcd.printf("%-16s", SHOW_NEW_TEXT ? LCDTextBot : LCDTextDoneBot);
}

char wait_for_char()
{
  int c;
  while (Serial.available() < 1)
    ; // wait for at least one  character to be available
  c = Serial.read();
  return (char)c;
}
