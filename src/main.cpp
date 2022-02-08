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

// PainlessMesh
#define   BLINK_PERIOD    3000 // milliseconds until cycle repeat
#define   BLINK_DURATION  200  // milliseconds LED is on for
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
#define BUTTON1       17
#define BUTTON2       16

// Prototypes
void sendMessage(); 
void receivedCallback(uint32_t from, String & msg);
void newConnectionCallback(uint32_t nodeId);
void changedConnectionCallback(); 
void nodeTimeAdjustedCallback(int32_t offset); 
void delayReceivedCallback(uint32_t from, int32_t delay);
void nextTrack();
void status();

Scheduler     userScheduler; // to control your personal task
painlessMesh  mesh;
Audio audio;
EasyButton nextButton(BUTTON1); // skip track
EasyButton statusButton(BUTTON2); // info

bool calc_delay = false;
int question_number = 0;
int breathing;
SimpleList<uint32_t> nodes;

void sendMessage() ; // Prototype
Task taskSendMessage( TASK_SECOND * 1, TASK_FOREVER, &sendMessage ); // start with a one second interval

// Task to blink the number of nodes
Task blinkNoNodes;
bool onFlag = false;

void setup() {
  Serial.begin(115200);

  // UI controls
  pinMode(LED_STATUS, OUTPUT);
  ledcSetup(0, 1000, 10);   // chan 0, 1kHz, 10-bit
  ledcAttachPin(LED_LEVEL, 0); // chan 0
  nextButton.begin();
  nextButton.onPressed(nextTrack);
  statusButton.begin();
  statusButton.onPressed(status);

  // audio setup
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  SD.begin(SD_CS);
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(17); // 0...21

  // mesh setup
  mesh.setDebugMsgTypes(ERROR | DEBUG);  // set before init() so that you can see error messages
  mesh.init(MESH_SSID, MESH_PASSWORD, &userScheduler, MESH_PORT);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);
  mesh.onNodeDelayReceived(&delayReceivedCallback);

  userScheduler.addTask( taskSendMessage );
  taskSendMessage.enable();

  blinkNoNodes.set(BLINK_PERIOD, (mesh.getNodeList().size() + 1) * 2, []() {
      // If on, switch off, else switch on
      if (onFlag)
        onFlag = false;
      else
        onFlag = true;
      blinkNoNodes.delay(BLINK_DURATION);

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
  int breathing = abs((int)millis() % 2000 - 1000)/10;   // triangle wave, 0.5Hz, 0..100
  ledcWrite(0, (int) max(0, (int)(mesh.stability*0.9) - breathing));
}

// called when button pressed
void nextTrack(){
  char filename[80];
  sprintf(filename, "/%02d-answer.mp3", question_number+1); // filenames start with 1
  Serial.printf("button: next (%s)\n", filename);
  audio.connecttoFS(SD, filename); // start playback (async)  
  mesh.sendBroadcast(filename);
  question_number = (question_number + 1) % 26;  //  increment, limit to 0-25
}

void status(){
  Serial.println("-----------------");
  Serial.print("station id: ");
  Serial.println(mesh.getNodeId());
  Serial.printf("mesh time: %zu\n", mesh.getNodeTime());
  Serial.printf("mesh stability: %d\n", mesh.stability);
   
  nodes = mesh.getNodeList();
  Serial.printf("Num nodes: %d\n", nodes.size());
  Serial.printf("mesh nodes: ");
  SimpleList<uint32_t>::iterator node = nodes.begin();
  while (node != nodes.end()) {
    Serial.printf(" %u", *node);
    node++;
  }
  Serial.println();

  Serial.print("mesh sub-connections: ");
  Serial.printf("  JSON: %s\n", mesh.subConnectionJson().c_str());

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
  taskSendMessage.setInterval( random(TASK_SECOND * 3, TASK_SECOND * 6));  // between 1 and 5 seconds
}


void receivedCallback(uint32_t from, String & msg) {
  if (msg.startsWith("/")){
    Serial.printf("starting playback of %s", msg.c_str());
    audio.connecttoFS(SD, msg.c_str()); // start playback (async)  
  }
  Serial.printf("<-- Message from %u msg=%s\n", from, msg.c_str());
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

// i2s audio callbacks

void audio_info(const char *info){
    Serial.print("info        "); Serial.println(info);
}
void audio_id3data(const char *info){  //id3 metadata
    Serial.print("id3data     ");Serial.println(info);
}
void audio_eof_mp3(const char *info){  //end of file
    Serial.print("eof_mp3     ");Serial.println(info);
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