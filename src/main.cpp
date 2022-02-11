//************************************************************
// combining I2S audio, PainlessMesh and other things for art
//
// (mesh example:)
// 1. blinks led once for every node on the mesh
// 2. blink cycle repeats every BLINK_PERIOD
// 3. sends a silly message to every node on the mesh at a random time between 1 and 5 seconds
// 4. prints anything it receives to Serial.print
//
//
//************************************************************
#include "Arduino.h"
#include <EasyButton.h>
#include <painlessMesh.h>
#include "Audio.h"
#include "SD.h"
#include "FS.h"

#define ARTNET_VERSION "0.1"

// PainlessMesh
#define   BLINK_PERIOD    5000 // milliseconds until cycle repeat
#define   BLINK_DURATION  175  // milliseconds LED is on for
#define   MESH_SSID       "artnet"
#define   MESH_PASSWORD   "loh6eiRoo2Ahrie"
#define   MESH_PORT       5555

// Digital I/O used
#define SD_CS          5
#define SPI_MOSI      23
#define SPI_MISO      19
#define SPI_SCK       18
#define I2S_DOUT      25
#define I2S_BCLK      27
#define I2S_LRC       26
#define LED_STATUS     2
#define LED_LEVEL     15
#define BUTTON1       34
#define BUTTON2       35
#define KNOB          32
// I2C: 21 SDA / 22 SCL

// Stations
#define STA_CONTROLLER  3171316429
#define STA_QUESTIONS   535391373
#define STA_ANSWERS     164488417

// Prototypes
void sendMessage(); 
void receivedCallback(uint32_t from, String & msg);
void newConnectionCallback(uint32_t nodeId);
void changedConnectionCallback(); 
void nodeTimeAdjustedCallback(int32_t offset); 
void delayReceivedCallback(uint32_t from, int32_t delay);
void triggerEvent(String msg);
void nextQuestion();
void status();
void board_config();

Scheduler     userScheduler; // to control your personal task
painlessMesh  mesh;
Audio audio;
EasyButton nextButton(BUTTON1, 50, true); // skip track
EasyButton statusButton(BUTTON2, 50, true); // info

bool calc_delay = false;
SimpleList<uint32_t> nodes;

int question_number = 0;
char chipid[32];
int short_id;
int chaos_level = 0;  // glitch meter
bool is_controller, has_buttons, has_audio, has_knob, has_sd_card = false;

int mode = 0;  // 0=start 1=question 2=pause 3=answer
String modes[4] = {"start","question","pause","answer"};
#define MODE_START 0
#define MODE_QUESTION 1
#define MODE_PAUSE 2
#define MODE_ANSWER 3


void sendMessage() ; // Prototype
Task taskSendMessage( TASK_SECOND * 1, TASK_FOREVER, &sendMessage ); // start with a one second interval
// Task taskPauseMessage( delay , TASK_ONCE, &sendPause);

// Task to blink the number of nodes
Task blinkNoNodes;
bool onFlag = false;

// --------------------
void setup() {
  Serial.begin(115200);
  uint64_t addr = ESP.getEfuseMac(); // The chip ID is essentially its MAC address(length: 6 bytes).
  // uint16_t short_addr = (uint16_t)(addr >> 32);
  delay(500);
  sprintf(chipid, "%012llx", addr);
  Serial.printf("ArtNet v%s started. Node %s\n", ARTNET_VERSION, chipid);
  board_config();

  // UI controls
  pinMode(LED_STATUS, OUTPUT);
  ledcSetup(0, 1000, 10);   // chan 0, 1kHz, 10-bit
  ledcAttachPin(LED_LEVEL, 0); // chan 0
  nextButton.begin();
  nextButton.onPressed(nextQuestion);
  statusButton.begin();
  statusButton.onPressed(status);
  pinMode(KNOB, INPUT);

  // audio setup
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  has_sd_card = SD.begin(SD_CS);
  Serial.println(has_sd_card ? "SD card loaded." : "* ERROR: No SD card found!");
  if (has_audio){
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(19); // 0...21
  }

  // mesh setup
  mesh.setDebugMsgTypes(ERROR | DEBUG);  // set before init() so that you can see error messages
  mesh.init(MESH_SSID, MESH_PASSWORD, &userScheduler, MESH_PORT);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);
  mesh.onNodeDelayReceived(&delayReceivedCallback);

  // Task setup
  userScheduler.addTask( taskSendMessage );
  taskSendMessage.enable();

  blinkNoNodes.set(BLINK_PERIOD, (mesh.getNodeList().size() + 1) * 2, []() {
      // If on, switch off, else switch on
      if (onFlag)
        onFlag = false;
      else
        onFlag = true;
      blinkNoNodes.delay(BLINK_DURATION);

      if (random(4)==1 and chaos_level > 2){
        audio.setTimeOffset(1-random(3));
      } 

      if (blinkNoNodes.isLastIteration()) {
        // Finished blinking. Reset task for next run 
        // blink number of nodes (including this node) times
        blinkNoNodes.setIterations((mesh.getNodeList().size() + 1) * 2);
        // Calculate delay based on current mesh time and BLINK_PERIOD
        // This results in blinks between nodes being synced
        blinkNoNodes.enableDelayed(BLINK_PERIOD - 
            (mesh.getNodeTime() % (BLINK_PERIOD*1000))/1000);
      }
  });
  
  userScheduler.addTask(blinkNoNodes);
  blinkNoNodes.enable();

  randomSeed(analogRead(A0));
}

void loop() {
  mesh.update();
  audio.loop();
  nextButton.read();
  statusButton.read();
  digitalWrite(LED_STATUS, onFlag);
  int breathing = abs((int)millis() % 4000 - 2000)/6;   // triangle wave, 0.25Hz, 0..330
  int value = (int)(mesh.stability*0.7) - breathing;
  if (is_controller && (millis() % 400 == 0)){
    int reading = analogRead(KNOB) / 1000;
    if (chaos_level != reading){
      chaos_level = reading;
      mesh.sendBroadcast(String("chaos:") + chaos_level);
      Serial.printf("Chaos level set to %d\n", chaos_level);
    }
  } 
  ledcWrite(0, max(0, min(value, 1000)));
}

String mp3_filename(){
  char filename[40];
  sprintf(filename, "/%02d-%s.mp3", question_number, modes[mode].c_str());
  return(filename);
}

/*
    controller_node:
    - manages the timing and order of questions & answers
    - sends requests to play mp3
    - listens for messages saying mp3 playback is finished
    - pauses for a random time
    question_node / answer_node:
    - listens for events to playback
    - plays mp3s matching an expected pattern
    - sends message when playback is finished
*/

// called by button handler
void nextQuestion(){
  mode = MODE_START;
  triggerEvent("next");
}

// called when button pressed
void triggerEvent(String msg){
  Serial.println("triggerEvent");
  if (mode == MODE_START){
    // set up start conditions
    question_number = question_number % 26 + 1;  //  increment, limit to 0-25
    Serial.println(String("Start, question ")+question_number);
    if (chaos_level == 4){
      question_number = random(26);
    }
    // TODO initiate pause
    mode = MODE_QUESTION;
    // trigger playback of question mp3
    mesh.sendBroadcast(mp3_filename());     
    Serial.println(mp3_filename() + " requested for MODE_QUESTION");
    // after pause, trigger first question
  } else if (mode == MODE_QUESTION){
    // wait for 'mp3_eof' event
    if (msg.startsWith("eof_mp3")){
      Serial.println("Question playback done");
      mode = MODE_ANSWER;
      mesh.sendBroadcast(mp3_filename());
      Serial.println(mp3_filename() + " requested for MODE_ANSWER");
    }
  // TODO initiate pause
  } else if (mode == MODE_ANSWER){
    // wait for 'mp3_eof' event
    if (msg.startsWith("eof_mp3")){    
      Serial.println("Answer playback done");
      if (chaos_level > 2){
        mode = MODE_START;
      }
    }
    // TODO initiate pause
  }
  
  

}

void status(){
  Serial.println("-----------------");
  Serial.print("mesh node id: ");
  Serial.print(mesh.getNodeId());
  Serial.print(" artnet id: ");
  Serial.println(chipid);
  Serial.printf("mesh '%s' || stability: %d || mesh time: %zu\n", 
      MESH_SSID, 
      mesh.stability,
      mesh.getNodeTime());   
  nodes = mesh.getNodeList();
  Serial.printf("Num nodes: %d\n", nodes.size()+1);
  Serial.printf("mesh nodes: ");
  SimpleList<uint32_t>::iterator node = nodes.begin();
  while (node != nodes.end()) {
    Serial.printf(" %u", *node);
    node++;
  }
  Serial.println();
  Serial.print("mesh sub-connections: ");
  Serial.printf("  JSON: %s\n", mesh.subConnectionJson().c_str());
  if (chaos_level > 1){
    mesh.sendBroadcast("glitch");
  }
  Serial.println("-----------------");
}

void sendMessage() {
  String msg = "Status from node ";
  msg += mesh.getNodeId();
  msg += " myFreeMemory: " + String(ESP.getFreeHeap());
  mesh.sendBroadcast(msg);

  if (calc_delay) {
    SimpleList<uint32_t>::iterator node = nodes.begin();
    while (node != nodes.end()) {
      mesh.startDelayMeas(*node);
      node++;
    }
    calc_delay = false;
  }

  Serial.printf("Sending message: %s\n", msg.c_str());
  taskSendMessage.setInterval( random(TASK_SECOND * 3, TASK_SECOND * 5));  // between 1 and 5 seconds
}


void receivedCallback(uint32_t from, String & msg) {
  Serial.printf("<-- Message from %u msg=%s\n", from, msg.c_str());

  if (msg.startsWith("chaos:")){
    int newval = msg.substring(6).toInt();
    Serial.print("got chaos:");
    Serial.println(newval);
    chaos_level = newval;
  }
  if (msg.startsWith("glitch")){
    audio.setTimeOffset(chaos_level/2-random(chaos_level));
  }
  if (msg.startsWith("/")) {
    if (has_sd_card){
      Serial.printf("starting playback of %s\n", msg.c_str());
      audio.connecttoFS(SD, msg.c_str()); // start playback (async)  
    }
  } else if (msg.startsWith("eof_mp3")) {
    Serial.printf("Received eof_mp3 from %u", from);
    if (is_controller) triggerEvent(msg);
  }
 
  if (random(7-chaos_level)==1 && chaos_level > 0){
    int weird_factor = chaos_level * 20;
    audio.audioFileSeek((100 - random(weird_factor) + weird_factor/2)/100.0);
  }
}

void newConnectionCallback(uint32_t nodeId) {
  // Reset blink task
  onFlag = false;
  blinkNoNodes.setIterations((mesh.getNodeList().size() + 1) * 2);
  blinkNoNodes.enableDelayed(BLINK_PERIOD - (mesh.getNodeTime() % (BLINK_PERIOD*1000))/1000);
 
  Serial.printf("* New Connection, nodeId = %u\n", nodeId);
  Serial.printf("  JSON: %s\n", mesh.subConnectionJson(true).c_str());
}

void changedConnectionCallback() {
  Serial.printf("Changed connections\n");
  // Reset blink task
  onFlag = false;
  blinkNoNodes.setIterations((mesh.getNodeList().size() + 1) * 2);
  blinkNoNodes.enableDelayed(BLINK_PERIOD - (mesh.getNodeTime() % (BLINK_PERIOD*1000))/1000);
  status();
  calc_delay = true;
}

void nodeTimeAdjustedCallback(int32_t offset) {
  Serial.printf("Adjusted time %u. Offset = %d\n", mesh.getNodeTime(), offset);
}

void delayReceivedCallback(uint32_t from, int32_t delay) {
  Serial.printf("Delay to node %u is %d us\n", from, delay);
}

// board config
void board_config(){ 
  if (!strcmp(chipid, "cc7206bd9e7c")){ // controller node 
    short_id = 6429;
    has_audio = false;
    has_buttons = true;
    has_knob = true;
    is_controller = true;
  }
  if (!strcmp(chipid, "8c6ce91f9c9c")){ // breadboard line out node 1373
    short_id = 1373;
    has_audio = true;
    has_buttons = false;
    has_knob = false;
  }
  if (!strcmp(chipid, "e0e4cd09f0b8")){ // line out node 8417
    short_id = 8417;
    has_audio = true;
    has_buttons = false;
    has_knob = false;
  }
  // default question node
  if (!strcmp(chipid, "3c7506bd9e7c")){ // audio amp node 7053
    short_id = 7053;
    has_audio = true;
    has_buttons = false;
    has_knob = false;
  }  
  // default answer node
  if (!strcmp(chipid, "f463e91f9c9c")){ // audio amp 9173 
    short_id = 9173;
    has_audio = true;
    has_buttons = false;
    has_knob = false;
  }
  Serial.print("has_audio: ");
  Serial.println(has_audio ? "true" : "false");

}


// i2s audio callbacks

void audio_info(const char *info){
    Serial.print("info        "); Serial.println(info);
}
void audio_id3data(const char *info){  //id3 metadata
    Serial.print("id3data     ");Serial.println(info);
}
void audio_eof_mp3(const char *info){  //end of file
    Serial.print("eof_mp3     ");Serial.println(info);
    mesh.sendBroadcast("eof_mp3:");
}
void audio_showstation(const char *info){
    Serial.print("station     ");Serial.println(info);
}
void audio_showstreamtitle(const char *info){
    Serial.print("streamtitle ");Serial.println(info);
}
void audio_bitrate(const char *info){
    Serial.print("bitrate     ");Serial.println(info);
}
void audio_commercial(const char *info){  //duration in sec
    Serial.print("commercial  ");Serial.println(info);
}
void audio_icyurl(const char *info){  //homepage
    Serial.print("icyurl      ");Serial.println(info);
}
void audio_lasthost(const char *info){  //stream URL played
    Serial.print("lasthost    ");Serial.println(info);
}
void audio_eof_speech(const char *info){
    Serial.print("eof_speech  ");Serial.println(info);
}