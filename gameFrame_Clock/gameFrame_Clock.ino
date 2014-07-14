#include <SPI.h>
#include <EEPROM.h>
#include <RTClite.h>
#include <SdFat.h>
#include <IniFileLite.h>
#include <Adafruit_NeoPixel.h>

/***************************************************
  BMP parsing code based on example sketch for the Adafruit 
  1.8" SPI display library by Adafruit.
  http://www.adafruit.com/products/358

  "Probably Random" number generator from:
  https://gist.github.com/endolith/2568571
 ****************************************************/

#define SD_CS    9  // Chip select line for SD card
SdFat sd; // set filesystem
SdFile myFile; // set filesystem

#define BUFFPIXEL 1

// In the SD card, place 24 bit color BMP files (be sure they are 24-bit!)
// There are examples included

// Parameter 1 = number of pixels in strip
// Parameter 2 = pin number (most are valid)
// Parameter 3 = pixel type flags, add together as needed:
//   NEO_RGB     Pixels are wired for RGB bitstream
//   NEO_GRB     Pixels are wired for GRB bitstream
//   NEO_KHZ400  400 KHz bitstream (e.g. FLORA pixels)
//   NEO_KHZ800  800 KHz bitstream (e.g. High Density LED strip)
Adafruit_NeoPixel strip = Adafruit_NeoPixel(256, 6, NEO_GRB + NEO_KHZ800);

//Random Number Generator
byte sample = 0;
boolean sample_waiting = false;
byte current_bit = 0;
byte randomResult = 0;

//Button setup
const uint8_t buttonNextPin = 4; // "Next" button
const uint8_t buttonSetupPin = 5;  // "Setup" button

#define STATUS_LED 3

//Enable prints?
const boolean debugMode = false;

//System Setup
boolean
  logoPlayed = false, // plays logo animation correctly reardless of playMode
  folderLoop = true, // animation looping
  moveLoop = false, // translation/pan looping
  buttonPressed = false, // control button check
  buttonEnabled = true, // debounce guard
  setupActive = false, // showing menus (set brightness, playback mode, etc.)
  panoff = true, // movement scrolls off screen
  singleGraphic = false, // single BMP file
  abortImage = false, // image is corrupt; abort, retry, fail?
  verboseOutput = false, // output extra info to LEDs
  statusLedState = false, // flicker tech
  clockShown = false, // clock mode?
  clockSet = false, // have we set the time?
  clockSetBlink = false, // flicker digit
  clockDigitSet = false, // is hours/minutes set?
  enableSecondHand = true,
  clockAnimationActive = false, // currently showing clock anim
  hour12 = true; // 12-hour clock?
byte
  playMode = 0, // 0 = sequential, 1 = random, 2 = pause animations
  gameMode = 1, // breakout, ???
  brightness = 4, // LED brightness
  brightnessMultiplier = 10, // DO NOT CHANGE THIS
  cycleTimeSetting = 2, // time before next animation: 1=10 secs, 2=30 secs, 3=1 min... 8=infinity
  fpShield = 0, // button false positive shield
  setupMode = 0, // 0 = brightmess, 1 = play mode, 2 = cycle time
  lowestMem = 250, // storage for lowest number of available bytes
  clockAnimationLength = 5, // seconds to play clock animations
  secondHandX = 0,
  secondHandY = 0,
  secondOffset = 0,
  lastSecond = 255, // a moment ago
  currentHour = 12,
  currentMinute = 0,
  currentSecond = 0;
int
  secondCounter = 0, // counts up every second
  cycleTime = 30, // seconds to wait before progressing to next folder
  numFolders = 0, // number of folders on sd
  folderIndex = 0, // current folder
  chainIndex = -1, // for chaining multiple folders
  fileIndex = 0, // current frame
  offsetBufferX = 0, // for storing offset when entering menu
  offsetBufferY = 0, // for storing offset when entering menu
  offsetSpeedX = 0, // number of pixels to translate each frame
  offsetSpeedY = 0, // number of pixels to translate each frame
  offsetX = 0, // for translating images x pixels
  offsetY = 0, // for translating images y pixels
  imageWidth = 0,
  imageHeight = 0;
unsigned long
  secondHandColor = 0, // color grabbed from digits.bmp for second hand
  lastTime = 0, // yep
  drawTime = 0, // debugging time to read from sd
  holdTime = 200, // millisecods to hold each .bmp frame
  swapTime = 0, // system time to advance to next frame
  baseTime = 0, // system time logged at start of each new image sequence
  buttonTime = 0, // time the last button was pressed (debounce code)
  setupEndTime = 0, // pause animation while in setup mode
  setupEnterTime = 0; // time we enter setup
char
  chainRootFolder[9], // chain game
  nextFolder[21] = "00system/logo"; // dictated next animation

RTC_DS1307 rtc;
DateTime now;

void setup(void) {
  // debug LED setup
  pinMode(STATUS_LED, OUTPUT);
  analogWrite(STATUS_LED, 100);

  wdtSetup();
  
  pinMode(buttonNextPin, INPUT);    // button as input
  pinMode(buttonSetupPin, INPUT);    // button as input
  digitalWrite(buttonNextPin, HIGH); // turns on pull-up resistor after input
  digitalWrite(buttonSetupPin, HIGH); // turns on pull-up resistor after input
  
  if (debugMode == true)
  {
    Serial.begin(57600);
    printFreeRAM();
  }
  
  // init clock and begin counting seconds
  rtc.begin();
  adjustClock();

  byte output = 0;

  // load last settings
  // read brightness setting from EEPROM
  output = EEPROM.read(0);
  if (output >= 1 && output <= 7) brightness = output;
  
  // read playMode setting from EEPROM
  output = EEPROM.read(1);
  if (output >= 0 && output <= 2) playMode = output;

  // read cycleTimeSetting setting from EEPROM
  output = EEPROM.read(2);
  if (output >= 1 && output <= 8) cycleTimeSetting = output;
  setCycleTime();

  strip.begin();
  strip.show(); // Initialize all pixels to 'off'
  strip.setBrightness(brightness * brightnessMultiplier);

  // run burn in test if both buttons held on boot
  if ((digitalRead(buttonNextPin) == LOW) && (digitalRead(buttonSetupPin) == LOW))
  {
    // max brightness
    strip.setBrightness(7 * brightnessMultiplier);
    while (true)
    {
      testScreen();
    }
  }

  // revert to these values if setup button held on boot
  if (digitalRead(buttonSetupPin) == LOW)
  {
    brightness = 1;
    strip.setBrightness(brightness * brightnessMultiplier);
    playMode = 0;
    cycleTimeSetting = 2;
    setCycleTime();
  }
  
  // show test screens and folder count if next button held on boot
  if (digitalRead(buttonNextPin) == LOW)
  {
    verboseOutput = true;
    testScreen();
  }

  Serial.print(F("Init SD: "));
  if (!sd.begin(SD_CS, SPI_FULL_SPEED)) {
    Serial.println(F("fail"));
    // SD error message
    sdErrorMessage();
    return;
  }
  Serial.println(F("OK!"));

  char folder[9];
  
  // file indexes appear to loop after 2048
  for (int fileIndex=0; fileIndex<2048; fileIndex++)
  {
    myFile.open(sd.vwd(), fileIndex, O_READ);
    if (myFile.isDir()) {
      Serial.println(F("---"));
      if (verboseOutput == true)
      {
        strip.setPixelColor(numFolders, strip.Color(128, 255, 0));
        strip.show();
      }
      numFolders++;
      Serial.print(F("File Index: "));
      Serial.println(fileIndex);
      myFile.getFilename(folder);
      Serial.print(F("Folder: "));
      Serial.println(folder);
      myFile.close();
    }
    else myFile.close();
  }
  Serial.print(numFolders);
  Serial.println(F(" folders found."));
  if (verboseOutput == true)
  {
    delay(5000);
  }
  
  // play logo animation
  sd.chdir(nextFolder);
  readIniFile();
  drawFrame();
}

void testScreen()
{
  // white
  for (int i=0; i<256; i++)
  {
    strip.setPixelColor(i, strip.Color(255, 255, 255));
  }
  strip.show();
  delay(2000);

  // red
  for (int i=0; i<256; i++)
  {
    strip.setPixelColor(i, strip.Color(255, 0, 0));
  }
  strip.show();
  delay(2000);

  // green
  for (int i=0; i<256; i++)
  {
    strip.setPixelColor(i, strip.Color(0, 255, 0));
  }
  strip.show();
  delay(2000);

  // blue
  for (int i=0; i<256; i++)
  {
    strip.setPixelColor(i, strip.Color(0, 0, 255));
  }
  strip.show();
  delay(2000);
}

void sdErrorMessage()
{
  // red bars
  for (int index=64; index<80; index++)
  {
    strip.setPixelColor(index, strip.Color(255, 0, 0));
  }
  for (int index=80; index<192; index++)
  {
    strip.setPixelColor(index, strip.Color(0, 0, 0));
  }
  for (int index=192; index<208; index++)
  {
    strip.setPixelColor(index, strip.Color(255, 0, 0));
  }
  // S
  yellowDot(7, 6);
  yellowDot(6, 6);
  yellowDot(5, 6);
  yellowDot(4, 7);
  yellowDot(5, 8);
  yellowDot(6, 8);
  yellowDot(7, 9);
  yellowDot(6, 10);
  yellowDot(5, 10);
  yellowDot(4, 10);

  // D
  yellowDot(9, 6);
  yellowDot(10, 6);
  yellowDot(11, 7);
  yellowDot(11, 8);
  yellowDot(11, 9);
  yellowDot(10, 10);
  yellowDot(9, 10);
  yellowDot(9, 7);
  yellowDot(9, 8);
  yellowDot(9, 9);

  strip.setBrightness(brightness * brightnessMultiplier);
  strip.show();
  
  while (true)
  {
    for (int i=255; i>=0; i--)
    {
      analogWrite(STATUS_LED, i);
      delay(1);
    }
    for (int i=0; i<=254; i++)
    {
      analogWrite(STATUS_LED, i);
      delay(1);
    }
  }
}

void yellowDot(byte x, byte y)
{
  strip.setPixelColor(getIndex(x, y), strip.Color(255, 255, 0));
}

void setCycleTime()
{
  if (cycleTimeSetting == 2)
  {
    cycleTime = 30;
  }
  else if (cycleTimeSetting == 3)
  {
    cycleTime = 60;
  }
  else if (cycleTimeSetting == 4)
  {
    cycleTime = 300;
  }
  else if (cycleTimeSetting == 5)
  {
    cycleTime = 900;
  }
  else if (cycleTimeSetting == 6)
  {
    cycleTime = 1800;
  }
  else if (cycleTimeSetting == 7)
  {
    cycleTime = 3600;
  }
  else if (cycleTimeSetting == 8)
  {
    cycleTime = -1;
  }
  else
  {
    cycleTime = 10;
  }
}

void statusLedFlicker()
{
  if (statusLedState == false)
  {
    statusLedState = true;
    analogWrite(STATUS_LED, 254);
  }
  else
  {
    statusLedState = false;
    analogWrite(STATUS_LED, 255);
  }
}

void loop() {
    mainLoop();
}

void mainLoop()
{
  buttonDebounce();
  now = rtc.now();

  // next button
  if (digitalRead(buttonNextPin) == LOW && buttonPressed == false && buttonEnabled == true && !clockShown)
  {
    buttonPressed = true;
    if (setupActive == false)
    {
      // exit chaining if necessary
      if (chainIndex > -1)
      {
        chainIndex = -1;
        chainRootFolder[0] = '\0';
        sd.chdir("/");
      }
      nextImage();
      drawFrame();
    }
    else
    {
      setupEndTime = millis() + 3000;

      // adjust brightness
      if (setupMode == 0)
      {
        brightness += 1;
        if (brightness > 7) brightness = 1;
        char brightChar[2];
        char brightFile[23];
        strcpy_P(brightFile, PSTR("/00system/bright_"));
        itoa(brightness, brightChar, 10);
        strcat(brightFile, brightChar);
        strcat(brightFile, ".bmp");
        strip.setBrightness(brightness * brightnessMultiplier);
        bmpDraw(brightFile, 0, 0);
      }
      
      // adjust play mode
      else if (setupMode == 1)
      {
        playMode++;
        if (playMode > 2) playMode = 0;
        char playChar[2];
        char playFile[21];
        strcpy_P(playFile, PSTR("/00system/play_"));
        itoa(playMode, playChar, 10);
        strcat(playFile, playChar);
        strcat(playFile, ".bmp");
        bmpDraw(playFile, 0, 0);
      }

      // adjust cycle time
      else if (setupMode == 2)
      {
        cycleTimeSetting++;
        if (cycleTimeSetting > 8) cycleTimeSetting = 1;
        setCycleTime();
        char timeChar[2];
        char timeFile[21];
        strcpy_P(timeFile, PSTR("/00system/time_"));
        itoa(cycleTimeSetting, timeChar, 10);
        strcat(timeFile, timeChar);
        strcat(timeFile, ".bmp");
        bmpDraw(timeFile, 0, 0);
      }

      // clock mode
      else if (setupMode == 3)
      {
        initClock();
      }
    }
  }
  
  // reset clock
  if (digitalRead(buttonNextPin) == LOW && buttonPressed == false && buttonEnabled == true && clockShown)
  {
    buttonPressed = true;
    clockSet = false;
    clockSetBlink = false;
    getCurrentTime();
    setClock();
  }

  // setup button
  else if (digitalRead(buttonSetupPin) == LOW && buttonPressed == false && buttonEnabled == true)
  {
    buttonPressed = true;
    setupEndTime = millis() + 3000;
    
    if (setupActive == false)
    {
      setupActive = true;
      setupEnterTime = millis();
      offsetBufferX = offsetX;
      offsetBufferY = offsetY;
      offsetX = 0;
      offsetY = 0;
      if (myFile.isOpen()) myFile.close();
      if (clockShown || clockAnimationActive)
      {
        clockAnimationActive = false;
        clockShown = false;
        abortImage = true;
        nextFolder[0] = '\0';
      }
    }
    else
    {
      setupMode++;
      if (setupMode > 3) setupMode = 0;
    }
    if (setupMode == 0)
    {
      char brightChar[2];
      char brightFile[23];
      strcpy_P(brightFile, PSTR("/00system/bright_"));
      itoa(brightness, brightChar, 10);
      strcat(brightFile, brightChar);
      strcat(brightFile, ".bmp");
      bmpDraw(brightFile, 0, 0);
    }
    else if (setupMode == 1)
    {
      char playChar[2];
      char playFile[21];
      strcpy_P(playFile, PSTR("/00system/play_"));
      itoa(playMode, playChar, 10);
      strcat(playFile, playChar);
      strcat(playFile, ".bmp");
      bmpDraw(playFile, 0, 0);
    }
    else if (setupMode == 2)
    {
      char timeChar[2];
      char timeFile[21];
      strcpy_P(timeFile, PSTR("/00system/time_"));
      itoa(cycleTimeSetting, timeChar, 10);
      strcat(timeFile, timeChar);
      strcat(timeFile, ".bmp");
      bmpDraw(timeFile, 0, 0);
    }
    else if (setupMode == 3)
    {
      char gameChar[2];
      char gameFile[21];
      strcpy_P(gameFile, PSTR("/00system/clock.bmp"));
      bmpDraw(gameFile, 0, 0);
    }
  }

  // time to exit setup mode?
  if (setupActive == true)
  {
    if (millis() > setupEndTime)
    {
      setupActive = false;
      
      // save any new settings to EEPROM
      if (EEPROM.read(0) != brightness)
      {
        EEPROM.write(0, brightness);
      }
      if (EEPROM.read(1) != playMode)
      {
        EEPROM.write(1, playMode);
      }
      if (EEPROM.read(2) != cycleTimeSetting)
      {
        EEPROM.write(2, cycleTimeSetting);
      }
      
      // return to brightness setup next time
      setupMode = 0;

      offsetX = offsetBufferX;
      offsetY = offsetBufferY;
      if (playMode == 2)
      {
        offsetX = imageWidth / -2 + 8;
        offsetY = imageHeight / 2 - 8;
      }
      swapTime = swapTime + (millis() - setupEnterTime);
      baseTime = baseTime + (millis() - setupEnterTime);
      if (abortImage == false)
      {
        drawFrame();
      }
    }
  }
  
  // currently playing images?
  if (setupActive == false)
  {
    if (clockShown == false || clockAnimationActive == true)
    {
      // advance counter
      if (now.second() != currentSecond)
      {
        currentSecond = now.second();
        secondCounter++;
        // revert to clock display if animation played for 5 seconds
        if (clockAnimationActive == true && secondCounter >= clockAnimationLength)
        {
          initClock();
        }
      }
  
      // did image load fail?
      if (abortImage == true && clockShown == false)
      {
        abortImage = false;
        nextImage();
        drawFrame();
      }
  
      // progress to next folder if cycleTime is up
      // check for infinite mode
      else if (cycleTimeSetting != 8  && clockShown == false && clockAnimationActive == false)
      {
        if (secondCounter >= cycleTime)
        {
          nextImage();
          drawFrame();
        }
      }
  
      // animate if not a single-frame & animations are on
      if (holdTime != -1 && playMode != 2 || logoPlayed == false)
      {
        if (millis() >= swapTime && clockShown == false)
        {
          statusLedFlicker();
          swapTime = millis() + holdTime;
          fileIndex++;
          drawFrame();
        }
      }
    }
  
    // show clock
    else if (clockShown == true && clockAnimationActive == false)
    {
      showClock();
    }
  }
}

void nextImage()
{
  Serial.println(F("---"));
  Serial.println(F("Next Folder..."));
  if (myFile.isOpen()) myFile.close();
  boolean foundNewFolder = false;
  // reset secondCounter if not playing clock animations
  if (!clockAnimationActive) secondCounter = 0;
  baseTime = millis();
  holdTime = 0;
  char folder[9];
  sd.chdir("/");
  fileIndex = 0;
  offsetX = 0;
  offsetY = 0;
  singleGraphic = false;
  if (!logoPlayed) logoPlayed = true;
  
  // are we chaining folders?
  if (chainIndex > -1)
  {
    char chainChar[3];
    char chainDir[23];
    strcpy_P(chainDir, PSTR("/"));
    strcat(chainDir, chainRootFolder);
    strcat(chainDir, "/");
    itoa(chainIndex, chainChar, 10);
    strcat(chainDir, chainChar);
    if (sd.exists(chainDir))
    {
      Serial.print(F("Chaining: "));
      Serial.println(chainDir);
      sd.chdir(chainDir);
      chainIndex++;
    }
    else
    {
      // chaining concluded
      chainIndex = -1;
      chainRootFolder[0] = '\0';
      sd.chdir("/");
    }
  }

  // has the next animation has been dictated by the previous .INI file?
  if (nextFolder[0] != '\0' && chainIndex == -1)
  {
    Serial.print(F("Forcing next: "));
    Serial.println(nextFolder);
    if (sd.exists(nextFolder))
    {
      sd.chdir(nextFolder);
    }
    else
    {
      nextFolder[0] = '\0';
      Serial.println(F("Not exists!"));
    }
  }
  
  // next folder not assigned by .INI
  if (nextFolder[0] == '\0' && chainIndex == -1)
  {
    // Getting next folder
    // shuffle playback using "probably_random" code
    // https://gist.github.com/endolith/2568571
    if (playMode != 0) // check we're not in a sequential play mode
    {
      if (sample_waiting == true)
      {
        randomResult = rotl(randomResult, 1); // Spread randomness around
        randomResult ^= sample; // XOR preserves randomness
     
        current_bit++;
        if (current_bit > 7)
        {
          current_bit = 0;
        }
        
        while (randomResult > numFolders)
        {
          randomResult = randomResult - numFolders;
        }
      }
  
      int targetFolder = randomResult;
      
      // don't repeat the same image, please.
      if (targetFolder <= 0 or targetFolder == numFolders or targetFolder == numFolders - 1)
      {
        // Repeat image detected! Incrementing targetFolder.
        targetFolder = targetFolder + 2;
      }
  
      Serial.print(F("Randomly advancing "));
      Serial.print(targetFolder);
      Serial.println(F(" folder(s)."));
      int i = 1;
      while (i < targetFolder)
      {
        foundNewFolder = false;
        while (foundNewFolder == false)
        {
          myFile.open(sd.vwd(), folderIndex, O_READ);
          if (myFile.isDir()) {
            foundNewFolder = true;
            i++;
          }
          myFile.close();
          folderIndex++;
        }
      }
    }
  
    foundNewFolder = false;
  
    while (foundNewFolder == false)
    {
      myFile.open(sd.vwd(), folderIndex, O_READ);
      myFile.getFilename(folder);
      
      // ignore system folders that start with "00"
      if (myFile.isDir() && folder[0] != 48 && folder[1] != 48) {
        foundNewFolder = true;
        Serial.print(F("Folder Index: "));
        Serial.println(folderIndex);
        Serial.print(F("Opening Folder: "));
        Serial.println(folder);
  
        sd.chdir(folder);
        myFile.close();
      }
      else myFile.close();
      folderIndex++;
    }
  }
  
  // is this the start of a folder chain?
  char chainDir[2];
  strcpy_P(chainDir, PSTR("0"));
  if (sd.exists(chainDir))
  {
    Serial.print(F("Chaining detected: "));
    Serial.println(folder);
    memcpy(chainRootFolder, folder, 8);
    sd.chdir(chainDir);
    chainIndex = 1;
  }
  
  char firstImage[6];
  strcpy_P(firstImage, PSTR("0.bmp"));
  if (sd.exists(firstImage))
  {
    Serial.print(F("Opening File: "));
    Serial.print(folder);
    Serial.println(F("/config.ini"));
    readIniFile();
  
    char tmp[6];
    strcpy_P(tmp, PSTR("0.bmp"));
    refreshImageDimensions(tmp);
    
    Serial.print(F("Hold (in ms): "));
    Serial.println(holdTime);
    swapTime = millis() + holdTime;
    
    // setup image for x/y translation as needed if animations aren't paused
    if (playMode != 2)
    {
      if (offsetSpeedX > 0)
      {
        if (panoff == true) offsetX = (imageWidth * -1);
        else offsetX = (imageWidth * -1 + 16);
      }
      else if (offsetSpeedX < 0)
      {
        if (panoff == true) offsetX = 16;
        else offsetX = 0;
      }
      if (offsetSpeedY > 0)
      {
        if (panoff == true) offsetY = -16;
        else offsetY = 0;
      }
      else if (offsetSpeedY < 0)
      {
        if (panoff == true) offsetY = imageHeight;
        else offsetY = imageHeight - 16;
      }
    }
    // center image if animations are paused
    else
    {
      offsetX = imageWidth / -2 + 8;
      offsetY = imageHeight / 2 - 8;
    }
    
    // test for single frame
    
    char tmp_0[6];
    char tmp_1[6];
    strcpy_P(tmp_0, PSTR("0.bmp"));
    strcpy_P(tmp_1, PSTR("1.bmp"));
    if (sd.exists(tmp_0) && (!sd.exists(tmp_1)))
    {
      singleGraphic = true;
      // check for pan settings
      if (offsetSpeedX == 0 && offsetSpeedY == 0)
      {
        // single frame still
        holdTime = -1;
      }
    }
  }
  
  // empty folder
  else
  {
    Serial.println(F("Empty folder!"));
    nextImage();
  }

  // for some reason, leaving this in keeps the code from crashing. 
  // It MUST be stored PSTR. Bad Flash bytes?
  char antiCrash[9];
  strcpy_P(antiCrash, PSTR("c"));
}

void drawFrame()
{
  if (panoff == true)
  {
    if (offsetX > 16 || offsetX < (imageWidth * -1) || offsetY > imageHeight || offsetY < -16)
    {
      if (moveLoop == false)
      {
        fileIndex = 0;
        nextImage();
      }
      else
      {
        if (offsetSpeedX > 0 && offsetX >= 16)
        {
          offsetX = (imageWidth * -1);
        }
        else if (offsetSpeedX < 0 && offsetX <= imageWidth * -1)
        {
          offsetX = 16;
        }
        if (offsetSpeedY > 0 && offsetY >= imageHeight)
        {
          offsetY = -16;
        }
        else if (offsetSpeedY < 0 && offsetY <= -16)
        {
          offsetY = imageHeight;
        }
      }
    }
  }
  else
  {
    if (offsetX > 0 || offsetX < (imageWidth * -1 + 16) || offsetY > imageHeight - 16 || offsetY < 0)
    {
      if (moveLoop == false)
      {
        fileIndex = 0;
        nextImage();
      }
      else
      {
        if (offsetSpeedX > 0 && offsetX >= 0)
        {
          offsetX = (imageWidth * -1 + 16);
        }
        else if (offsetSpeedX < 0 && offsetX <= imageWidth - 16)
        {
          offsetX = 0;
        }
        if (offsetSpeedY > 0 && offsetY >= imageHeight - 16)
        {
          offsetY = 0;
        }
        else if (offsetSpeedY < 0 && offsetY <= 0)
        {
          offsetY = imageHeight - 16;
        }
      }
    }
  }
  if (singleGraphic == false)
  {
    char bmpFile[8]; // 3-digit number + .bmp + null byte
    itoa(fileIndex, bmpFile, 10);
    strcat(bmpFile, ".bmp");
    if (!sd.exists(bmpFile)) 
    {
      fileIndex = 0;
      itoa(fileIndex, bmpFile, 10);
      strcat(bmpFile, ".bmp");
      if (folderLoop == false)
      {
        nextImage();
      }
    }
    bmpDraw(bmpFile, 0, 0);
  }
  else bmpDraw("0.bmp", 0, 0);

  if (debugMode == true)
  {
    // print draw time in milliseconds
    drawTime = millis() - lastTime;
    lastTime = millis();
    Serial.print(F("ttd: "));
    Serial.println(drawTime);
  }
  if (offsetSpeedX != 0) offsetX += offsetSpeedX;
  if (offsetSpeedY != 0) offsetY += offsetSpeedY;
}

void refreshImageDimensions(char *filename) {

  const uint8_t  gridWidth = 16;
  const uint8_t  gridHeight = 16;

  if((0 >= gridWidth) || (0 >= gridHeight)) {
    Serial.print(F("Abort."));
    return;
  }
  
  // storing dimentions for image
  
  // Open requested file on SD card
  if (!myFile.open(filename, O_READ)) {
    Serial.println(F("File open failed"));
    sdErrorMessage();
    return;
  }

  // Parse BMP header
  if(read16(myFile) == 0x4D42) { // BMP signature
    (void)read32(myFile); // Read & ignore file size
    (void)read32(myFile); // Read & ignore creator bytes
    (void)read32(myFile); // skip data
    // Read DIB header
    (void)read32(myFile); // Read & ignore Header size
    imageWidth  = read32(myFile);
    imageHeight = read32(myFile);
    Serial.print(F("Image resolution: "));
    Serial.print(imageWidth);
    Serial.print(F("x"));
    Serial.println(imageHeight);
  }
  Serial.println(F("Closing Image..."));
  myFile.close();
}

// This function opens a Windows Bitmap (BMP) file and
// displays it at the given coordinates.  It's sped up
// by reading many pixels worth of data at a time
// (rather than pixel by pixel).  Increasing the buffer
// size takes more of the Arduino's precious RAM but
// makes loading a little faster.  20 pixels seems a
// good balance.

void bmpDraw(char *filename, uint8_t x, uint8_t y) {

  int  bmpWidth, bmpHeight;   // W+H in pixels
  uint8_t  bmpDepth;              // Bit depth (currently must be 24)
  uint32_t bmpImageoffset;        // Start of image data in file
  uint32_t  rowSize;               // Not always = bmpWidth; may have padding
  uint8_t  sdbuffer[3*BUFFPIXEL]; // pixel buffer (R+G+B per pixel)
  uint8_t  buffidx = sizeof(sdbuffer); // Current position in sdbuffer
  boolean  goodBmp = false;       // Set to true on valid header parse
  boolean  flip    = true;        // BMP is stored bottom-to-top
  int  w, h, row, col;
  uint8_t  r, g, b;
  uint32_t pos = 0;
  const uint8_t  gridWidth = 16;
  const uint8_t  gridHeight = 16;

  if((x >= gridWidth) || (y >= gridHeight)) {
    Serial.print(F("Abort."));
    return;
  }
  
  Serial.println();
  
  if (!myFile.isOpen())
  {
    Serial.print(F("Loading image '"));
    Serial.print(filename);
    Serial.println('\'');
    // Open requested file on SD card
    if (!myFile.open(filename, O_READ)) {
      Serial.println(F("File open failed"));
      sdErrorMessage();
      return;
    }
  }
  else myFile.rewind();
  
  if (debugMode == true)
  {
    printFreeRAM();
  }

  // Parse BMP header
  if(read16(myFile) == 0x4D42) { // BMP signature
    Serial.print(F("File size: ")); Serial.println(read32(myFile));
    (void)read32(myFile); // Read & ignore creator bytes
    bmpImageoffset = read32(myFile); // Start of image data
    Serial.print(F("Image Offset: ")); Serial.println(bmpImageoffset, DEC);
    // Read DIB header
    Serial.print(F("Header size: ")); Serial.println(read32(myFile));
    bmpWidth  = read32(myFile);
    bmpHeight = read32(myFile);
    if(read16(myFile) == 1) { // # planes -- must be '1'
      bmpDepth = read16(myFile); // bits per pixel
      Serial.print(F("Bit Depth: ")); Serial.println(bmpDepth);
      if((bmpDepth == 24) && (read32(myFile) == 0)) { // 0 = uncompressed

        goodBmp = true; // Supported BMP format -- proceed!
        Serial.print(F("Image size: "));
        Serial.print(bmpWidth);
        Serial.print('x');
        Serial.println(bmpHeight);

        Serial.print(F("Image offset: "));
        Serial.print(offsetX);
        Serial.print(F(", "));
        Serial.println(offsetY);

        // image smaller than 16x16?
        if ((bmpWidth < 16 && bmpWidth > -16) || (bmpHeight < 16 && bmpHeight > -16))
        {
          clearStripBuffer();
        }

        // BMP rows are padded (if needed) to 4-byte boundary
        rowSize = (bmpWidth * 3 + 3) & ~3;
        Serial.print(F("Row size: "));
        Serial.println(rowSize);

        // If bmpHeight is negative, image is in top-down order.
        // This is not canon but has been observed in the wild.
        if(bmpHeight < 0) {
          bmpHeight = -bmpHeight;
          flip      = false;
        }
        
        // initialize our pixel index
        byte index = 0; // a byte is perfect for a 16x16 grid

        // Crop area to be loaded
        w = bmpWidth;
        h = bmpHeight;
        if((x+w-1) >= gridWidth)  w = gridWidth - x;
        if((y+h-1) >= gridHeight) h = gridHeight - y;

        for (row=0; row<h; row++) { // For each scanline...

          // Seek to start of scan line.  It might seem labor-
          // intensive to be doing this on every line, but this
          // method covers a lot of gritty details like cropping
          // and scanline padding.  Also, the seek only takes
          // place if the file position actually needs to change
          // (avoids a lot of cluster math in SD library).
          
          if(flip) // Bitmap is stored bottom-to-top order (normal BMP)
            pos = (bmpImageoffset + (offsetX * -3) + (bmpHeight - 1 - (row + offsetY)) * rowSize);
          else     // Bitmap is stored top-to-bottom
            pos = bmpImageoffset + row * rowSize;
          if(myFile.curPosition() != pos) { // Need seek?
            myFile.seekSet(pos);
            buffidx = sizeof(sdbuffer); // Force buffer reload
          }
          
          for (col=0; col<w; col++) { // For each pixel...
            // Time to read more pixel data?
            if (buffidx >= sizeof(sdbuffer)) { // Indeed
              myFile.read(sdbuffer, sizeof(sdbuffer));
              buffidx = 0; // Set index to beginning
            }

            // push to LED buffer 
            b = sdbuffer[buffidx++];
            g = sdbuffer[buffidx++];
            r = sdbuffer[buffidx++];

            // offsetY is beyond bmpHeight
            if (row >= bmpHeight - offsetY)
            {
              // black pixel
              strip.setPixelColor(getIndex(col, row), strip.Color(0, 0, 0));
            }
            // offsetY is negative
            else if (row < offsetY * -1)
            {
              // black pixel
              strip.setPixelColor(getIndex(col, row), strip.Color(0, 0, 0));
            }
            // offserX is beyond bmpWidth
            else if (col >= bmpWidth + offsetX)
            {
              // black pixel
              strip.setPixelColor(getIndex(col, row), strip.Color(0, 0, 0));
            }
            // offsetX is positive
            else if (col < offsetX)
            {
              // black pixel
              strip.setPixelColor(getIndex(col, row), strip.Color(0, 0, 0));
            }
            // all good
            else strip.setPixelColor(getIndex(col+x, row), strip.Color(r, g, b));
            // paint pixel color
          } // end pixel
        } // end scanline
      } // end goodBmp
    }
  }
  if (!clockShown)
  {
    strip.show();
    // NOTE: strip.show() halts all interrupts, including the system clock.
    // Each call results in about 6825 microseconds lost to the void.
  }
  if (singleGraphic == false || setupActive == true)
  {
    Serial.println(F("Closing Image..."));
    myFile.close();
  }
  if(!goodBmp) Serial.println(F("Format unrecognized"));
}

byte getIndex(byte x, byte y)
{
  byte index;
  if (y == 0)
  {
    index = 15 - x;
  }
  else if (y % 2 != 0)
  {
    index = y * 16 + x;
  }
  else
  {
    index = (y * 16 + 15) - x;
  }
  return index;
}

void clearStripBuffer()
{
  for (int i=0; i<256; i++)
  {
    strip.setPixelColor(i, strip.Color(0, 0, 0));
  }
}

void buttonDebounce()
{
  // button debounce -- no false positives
  if (((digitalRead(buttonSetupPin) == HIGH) && digitalRead(buttonNextPin) == HIGH) && buttonPressed == true)
  {
    buttonPressed = false;
    buttonEnabled = false;
    buttonTime = millis();
  }
  if ((buttonEnabled == false) && buttonPressed == false)
  {
    if (millis() > buttonTime + 50) buttonEnabled = true;
  }
}

// Clock

void debugClockDisplay()
{
  // digital clock display of the time
  Serial.print(currentHour);
  printDigits(currentMinute);
  printDigits(currentSecond);
  Serial.println();
}

void printDigits(int digits)
{
  // utility function for clock display: prints preceding colon and leading 0
  Serial.print(":");
  if(digits < 10)
    Serial.print('0');
  Serial.print(digits);
}

void initClock()
{
  if (EEPROM.read(0) != brightness)
  {
    EEPROM.write(0, brightness);
  }
  if (EEPROM.read(1) != playMode)
  {
    EEPROM.write(1, playMode);
  }
  if (EEPROM.read(2) != cycleTimeSetting)
  {
    EEPROM.write(2, cycleTimeSetting);
  }
  
  secondCounter = 0;
  setupActive = false;
  clockShown = true;
  clockAnimationActive = false;
  buttonTime = millis();
  setupMode = 0;
  lastSecond = 255;
  
  if (myFile.isOpen()) myFile.close();
  holdTime = 0;
  sd.chdir("/");
  fileIndex = 0;
  singleGraphic = true;
  if (!logoPlayed) logoPlayed = true;
  
  readClockIni();

  if (!clockSet)
  {
    currentHour = 12;
    currentMinute = 0;
    setClock();
  }
  if (!enableSecondHand)
  {
    drawDigits();
    strip.show();
  }
}

void setClock()
{
  clockSet = false;
  currentSecond = 0;
  setClockHour();
  setClockMinute();
  adjustClock();
  lastSecond = 255;
  now = rtc.now();
}

void setClockHour()
{
  // rtc.adjust(DateTime(__DATE__, __TIME__));
  // set hour
  while (clockDigitSet == false)
  {
    drawDigits();
    strip.show();
    buttonDebounce();

    // setup button
    if (digitalRead(buttonSetupPin) == LOW && buttonPressed == false && buttonEnabled == true && clockSetBlink)
    {
      currentHour++;

      // remove the following two "hour12" lines to allow setting 24 hour clock in 12 hour mode
      // (if you add features that require AM/PM awareness)
      if (hour12 && currentHour > 12) currentHour -= 12;
      if (hour12 && currentHour == 0) currentHour = 12;

      if (currentHour > 23) currentHour = 0;
    }

    // next button
    if (digitalRead(buttonNextPin) == LOW && buttonPressed == false && buttonEnabled == true)
    {
      buttonPressed = true;
      clockDigitSet = true;
    }
  }
}
  
void setClockMinute()
{
  // set minutes
  while (clockDigitSet == true)
  {
    drawDigits();
    strip.show();
    buttonDebounce();

    // setup button
    if (digitalRead(buttonSetupPin) == LOW && buttonPressed == false && buttonEnabled == true && clockSetBlink)
    {
      currentMinute++;
      if (currentMinute > 59) currentMinute = 0;
    }

    // next button
    if (digitalRead(buttonNextPin) == LOW && buttonPressed == false && buttonEnabled == true)
    {
      buttonPressed = true;
      clockDigitSet = false;
      clockSetBlink = true;
    }
  }
}

void adjustClock()
{
  rtc.adjust(DateTime(2014, 1, 1, currentHour, currentMinute, currentSecond));
}

void getCurrentTime()
{
  currentHour = now.hour();
  currentMinute = now.minute();
  currentSecond = now.second();
}

void showClock()
{
  getCurrentTime();
  if (currentSecond != lastSecond)
  {
    secondCounter++;
    lastSecond = currentSecond;
    statusLedFlicker();

    // 24 hour conversion
    if (hour12 && currentHour > 12) currentHour -= 12;
    if (hour12 && currentHour == 0) currentHour = 12;
    if (debugMode == true)
    {
      debugClockDisplay();
      printFreeRAM();
    }

    // draw time
    if (enableSecondHand)
    {
      // offset second hand if required
      currentSecond = currentSecond + secondOffset;
      if (currentSecond >= 60) currentSecond -= 60;
      storeSecondHandColor();
      drawDigits();
      secondHand();
      strip.show();
    }
    // second hand disabled, so only draw time on new minute
    else if (currentSecond == 0)
    {
      drawDigits();
      strip.show();
    }

    // show an animation
    if (cycleTime != -1 && clockAnimationLength > 0 && (secondsIntoHour() % cycleTime) == 0 && clockSet == true)
    {
      secondCounter = 0;
      currentSecond = now.second();
      clockAnimationActive = true;
      clockShown = false;
      if (myFile.isOpen()) myFile.close();
      abortImage = true;
      nextFolder[0] = '\0';
    }
    // this boolean is set here to avoid showing an animation immediately after clock being set
    else if (!clockSet) clockSet = true;
  }
}

int secondsIntoHour()
{
  return (currentMinute * 60) + currentSecond;
}

void drawDigits()
{
  clockDigit_1();
  clockDigit_2();
  clockDigit_3();
  clockDigit_4();
  if (!clockSet) clockSetBlink = !clockSetBlink;
  if (myFile.isOpen()) myFile.close();
}

void storeSecondHandColor()
{
  getSecondHandIndex();
  offsetX = 0;
  offsetY = 176;
  strcpy_P(nextFolder, PSTR("/00system/digits.bmp"));
  // max brightness in order to store correct color value
  strip.setBrightness(255);
  bmpDraw(nextFolder, 0, 0);
  secondHandColor = strip.getPixelColor(getIndex(secondHandX, secondHandY));
  // restore brightness
  strip.setBrightness(brightness * brightnessMultiplier);
}

void clockDigit_1()
{
  char numChar[3];
  itoa(currentHour, numChar, 10);
  byte singleDigit = numChar[0] - '0';
  offsetX = -1;
  if (currentHour >= 10)
  {
    offsetY = singleDigit * 16;
  }
  else offsetY = 160;
  if (!clockSet && !clockDigitSet && !clockSetBlink)
  {
    offsetY = 160;
  }
  strcpy_P(nextFolder, PSTR("/00system/digits.bmp"));
  bmpDraw(nextFolder, 0, 0);
}

void clockDigit_2()
{
  char numChar[3];
  itoa(currentHour, numChar, 10);
  byte singleDigit;
  if (currentHour >= 10)
  {
    singleDigit = numChar[1] - '0';
  }
  else singleDigit = numChar[0] - '0';
  offsetX = 0;
  offsetY = singleDigit * 16;
  if (!clockSet && !clockDigitSet && !clockSetBlink)
  {
    offsetY = 160;
  }
  strcpy_P(nextFolder, PSTR("/00system/digits.bmp"));
  bmpDraw(nextFolder, 3, 0);
}

void clockDigit_3()
{
  char numChar[3];
  itoa(currentMinute, numChar, 10);
  byte singleDigit;
  if (currentMinute >= 10)
  {
    singleDigit = numChar[0] - '0';
  }
  else singleDigit = 0;
  offsetY = singleDigit * 16;
  if (!clockSet && clockDigitSet && !clockSetBlink)
  {
    offsetY = 160;
  }
  strcpy_P(nextFolder, PSTR("/00system/digits.bmp"));
  bmpDraw(nextFolder, 8, 0);
}

void clockDigit_4()
{
  char numChar[3];
  itoa(currentMinute, numChar, 10);
  byte singleDigit;
  if (currentMinute >= 10)
  {
    singleDigit = numChar[1] - '0';
  }
  else singleDigit = numChar[0] - '0';
  offsetY = singleDigit * 16;
  if (!clockSet && clockDigitSet && !clockSetBlink)
  {
    offsetY = 160;
  }
  strcpy_P(nextFolder, PSTR("/00system/digits.bmp"));
  bmpDraw(nextFolder, 12, 0);
}

void getSecondHandIndex()
{
  if (currentSecond < 16)
  {
    secondHandX = currentSecond;
    secondHandY = 0;
  }
  else if (currentSecond >= 16 && currentSecond < 30)
  {
    secondHandX = 15;
    secondHandY = (currentSecond - 15);
  }
  else if (currentSecond >= 30 && currentSecond < 46)
  {
    secondHandX = (15 - (currentSecond - 30));
    secondHandY = 15;
  }
  else if (currentSecond >= 46)
  {
    secondHandX = 0;
    secondHandY = (15 - (currentSecond - 45));
  }
}

void secondHand()
{
  getSecondHandIndex();
  strip.setPixelColor(getIndex(secondHandX, secondHandY), secondHandColor);
}

// .INI file support

void printErrorMessage(uint8_t e, bool eol = true)
{
  switch (e) {
  case IniFile::errorNoError:
    Serial.print(F("no error"));
    break;
  case IniFile::errorFileNotFound:
    Serial.print(F("fnf"));
    break;
  case IniFile::errorFileNotOpen:
    Serial.print(F("fno"));
    break;
  case IniFile::errorBufferTooSmall:
    Serial.print(F("bts"));
    break;
  case IniFile::errorSeekError:
    Serial.print(F("se"));
    break;
  case IniFile::errorSectionNotFound:
    Serial.print(F("snf"));
    break;
  case IniFile::errorKeyNotFound:
    Serial.print(F("knf"));
    break;
  case IniFile::errorEndOfFile:
    Serial.print(F("eof"));
    break;
  case IniFile::errorUnknownError:
    Serial.print(F("unknown"));
    break;
  default:
    Serial.print(F("unknown error value"));
    break;
  }
  if (eol)
    Serial.println();
}

void readIniFile()
{
  const size_t bufferLen = 50;
  char buffer[bufferLen];
  char configFile[11];
  strcpy_P(configFile, PSTR("config.ini"));
  const char *filename = configFile;
  IniFile ini(filename);
  if (!ini.open()) {
    Serial.print(filename);
    Serial.println(F(" does not exist"));
    // Cannot do anything else
  }
  else
  {
    Serial.println(F("Ini file exists"));
  }

  // Check the file is valid. This can be used to warn if any lines
  // are longer than the buffer.
  if (!ini.validate(buffer, bufferLen)) {
    Serial.print(F("ini file "));
    Serial.print(ini.getFilename());
    Serial.print(F(" not valid: "));
    printErrorMessage(ini.getError());
    // Cannot do anything else
  }
  char section[10];
  strcpy_P(section, PSTR("animation"));
  char entry[11];
  strcpy_P(entry, PSTR("hold"));

  // Fetch a value from a key which is present
  if (ini.getValue(section, entry, buffer, bufferLen)) {
    Serial.print(F("hold value: "));
    Serial.println(buffer);
    holdTime = atol(buffer);
  }
  else {
    printErrorMessage(ini.getError());
    holdTime = 200;
  }
  
  strcpy_P(entry, PSTR("loop"));

  // Fetch a boolean value
  bool loopCheck;
  bool found = ini.getValue(section, entry, buffer, bufferLen, loopCheck);
  if (found) {
    Serial.print(F("animation loop value: "));
    // Print value, converting boolean to a string
    Serial.println(loopCheck ? F("TRUE") : F("FALSE"));
    folderLoop = loopCheck;
  }
  else {
    printErrorMessage(ini.getError());
    folderLoop = true;
  }
  
  strcpy_P(section, PSTR("translate"));
  strcpy_P(entry, PSTR("moveX"));

  // Fetch a value from a key which is present
  if (ini.getValue(section, entry, buffer, bufferLen)) {
    Serial.print(F("moveX value: "));
    Serial.println(buffer);
    offsetSpeedX = atoi(buffer);
  }
  else {
    printErrorMessage(ini.getError());
    offsetSpeedX = 0;
  }
  
  strcpy_P(entry, PSTR("moveY"));

  // Fetch a value from a key which is present
  if (ini.getValue(section, entry, buffer, bufferLen)) {
    Serial.print(F("moveY value: "));
    Serial.println(buffer);
    offsetSpeedY = atoi(buffer);
  }
  else {
    printErrorMessage(ini.getError());
    offsetSpeedY = 0;
  }
  
  strcpy_P(entry, PSTR("loop"));

  // Fetch a boolean value
  bool loopCheck2;
  bool found2 = ini.getValue(section, entry, buffer, bufferLen, loopCheck2);
  if (found2) {
    Serial.print(F("translate loop value: "));
    // Print value, converting boolean to a string
    Serial.println(loopCheck2 ? F("TRUE") : F("FALSE"));
    moveLoop = loopCheck2;
  }
  else {
    printErrorMessage(ini.getError());
    moveLoop = false;
  }

  strcpy_P(entry, PSTR("panoff"));

  // Fetch a boolean value
  bool loopCheck3;
  bool found3 = ini.getValue(section, entry, buffer, bufferLen, loopCheck3);
  if (found3) {
    Serial.print(F("panoff value: "));
    // Print value, converting boolean to a string
    Serial.println(loopCheck3 ? F("TRUE") : F("FALSE"));
    panoff = loopCheck3;
  }
  else {
    printErrorMessage(ini.getError());
    panoff = true;
  }

  strcpy_P(entry, PSTR("nextFolder"));

  // Fetch a value from a key which is present
  if (ini.getValue(section, entry, buffer, bufferLen)) {
    Serial.print(F("nextFolder value: "));
    Serial.println(buffer);
    memcpy(nextFolder, buffer, 8);
  }
  else {
    printErrorMessage(ini.getError());
    nextFolder[0] = '\0';
  }
  
  if (ini.isOpen()) ini.close();
}

void readClockIni()
{
  const size_t bufferLen = 50;
  char buffer[bufferLen];
  char configFile[11];
  strcpy_P(configFile, PSTR("clock.ini"));
  const char *filename = configFile;
  IniFile ini(filename);
  sd.chdir("/00system");
  if (!ini.open()) {
    Serial.print(filename);
    Serial.println(F(" does not exist"));
    // Cannot do anything else
  }
  else
  {
    Serial.println(F("Ini file exists"));
  }

  // Check the file is valid. This can be used to warn if any lines
  // are longer than the buffer.
  if (!ini.validate(buffer, bufferLen)) {
    Serial.print(F("ini file "));
    Serial.print(ini.getFilename());
    Serial.print(F(" not valid: "));
    printErrorMessage(ini.getError());
    // Cannot do anything else
  }
  char section[10];
  strcpy_P(section, PSTR("clock"));
  char entry[11];
  strcpy_P(entry, PSTR("hour12"));

  // Fetch a boolean value
  // 12/24 hour mode
  bool loopCheck;
  bool found = ini.getValue(section, entry, buffer, bufferLen, loopCheck);
  if (found) {
    Serial.print(F("hour12 value: "));
    // Print value, converting boolean to a string
    Serial.println(loopCheck ? F("TRUE") : F("FALSE"));
    hour12 = loopCheck;
  }
  else {
    printErrorMessage(ini.getError());
    hour12 = true;
  }
  
  strcpy_P(entry, PSTR("second"));

  // Fetch a boolean value
  // show second hand
  found = ini.getValue(section, entry, buffer, bufferLen, loopCheck);
  if (found) {
    Serial.print(F("second value: "));
    // Print value, converting boolean to a string
    Serial.println(loopCheck ? F("TRUE") : F("FALSE"));
    enableSecondHand = loopCheck;
  }
  else {
    printErrorMessage(ini.getError());
    enableSecondHand = true;
  }
  
  strcpy_P(entry, PSTR("offset"));

  // Fetch a value from a key which is present
  // second hand position offset
  if (ini.getValue(section, entry, buffer, bufferLen)) {
    Serial.print(F("offset value: "));
    Serial.println(buffer);
    secondOffset = atoi(buffer);
  }
  else {
    printErrorMessage(ini.getError());
    secondOffset = 0;
  }

  strcpy_P(entry, PSTR("anim"));

  // Fetch a value from a key which is present
  // clock animation length in seconds
  if (ini.getValue(section, entry, buffer, bufferLen)) {
    Serial.print(F("anim value: "));
    Serial.println(buffer);
    clockAnimationLength = atoi(buffer);
  }
  else {
    printErrorMessage(ini.getError());
    clockAnimationLength = 5;
  }

  if (ini.isOpen()) ini.close();
  sd.chdir("/");
}

// These read 16- and 32-bit types from the SD card file.
// BMP data is stored little-endian, Arduino is little-endian too.
// May need to reverse subscript order if porting elsewhere.

uint16_t read16(SdFile& f) {
  uint16_t result;
  ((uint8_t *)&result)[0] = f.read(); // LSB
  ((uint8_t *)&result)[1] = f.read(); // MSB
  return result;
}

uint32_t read32(SdFile& f) {
  uint32_t result;
  ((uint8_t *)&result)[0] = f.read(); // LSB
  ((uint8_t *)&result)[1] = f.read();
  ((uint8_t *)&result)[2] = f.read();
  ((uint8_t *)&result)[3] = f.read(); // MSB
  return result;
}

// available RAM checker

void printFreeRAM()
{
  Serial.print(F("FreeRam: "));
  Serial.println(freeRam());
  if (freeRam() < lowestMem) lowestMem = freeRam();
  Serial.print(F("Lowest FreeRam: "));
  Serial.println(lowestMem);
}

int freeRam () {
  extern int __heap_start, *__brkval; 
  int v; 
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}

// Random Number Generation ("probably_random")
// https://gist.github.com/endolith/2568571
// Rotate bits to the left
// https://en.wikipedia.org/wiki/Circular_shift#Implementing_circular_shifts
byte rotl(const byte value, int shift) {
  if ((shift &= sizeof(value)*8 - 1) == 0)
    return value;
  return (value << shift) | (value >> (sizeof(value)*8 - shift));
}
 
// Setup of the watchdog timer.
void wdtSetup() {
  cli();
  MCUSR = 0;
  
  /* Start timed sequence */
  WDTCSR |= _BV(WDCE) | _BV(WDE);
 
  /* Put WDT into interrupt mode */
  /* Set shortest prescaler(time-out) value = 2048 cycles (~16 ms) */
  WDTCSR = _BV(WDIE);
 
  sei();
}
 
// Watchdog Timer Interrupt Service Routine
ISR(WDT_vect)
{
  sample = TCNT1L; // Ignore higher bits
  sample_waiting = true;
}
