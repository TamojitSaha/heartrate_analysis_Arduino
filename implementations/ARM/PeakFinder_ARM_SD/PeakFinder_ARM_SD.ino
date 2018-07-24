/*
 * Arduino Heart Rate Analysis Toolbox - Peak Finder ARM
 *      Copyright (C) 2018 Paul van Gent
 *      
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License V3 as published by
 * the Free Software Foundation. The program is free for any commercial and
 * non-commercial usage and adaptation, granted you give the recipients 
 * of your code the same open-source rights and license.
 * 
 * You can read the full license granted to you here:
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 * 
 * Please add the following citation to any work utilising one or more of the
 * implementation from this project:
 * 
 * <Add JORS paper reference once published>
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

// -------------------- Includes --------------------
#include <SD.h>

// -------------------- User Settable Variables --------------------
int8_t hrpin = 0; //Whatever analog pin the sensor is hooked up to
const int16_t sample_rate = 1000;
int8_t report_hr = 1; //if 1, reports raw heart rate and peak threshold data as well, else set to 0 (default 0)
float max_bpm = 180; //The max BPM to be expected, used in error detection (default 180)
float min_bpm = 45; //The min BPM to be expected, used in error detection (default 45)

// -------------------- Non-Settable Variables --------------------
// Seriously, don't touch
int largestVal = 0;
int largestValPos = 0;
int clippingcount = 0;
int8_t clipFlag = 0;
int clipStart = 0;
int8_t clipEnd = 0;
int lastVal = 0;
int16_t max_RR = (60.0 / min_bpm) * 1000.0;
int16_t min_RR = (60.0 / max_bpm) * 1000.0;
const int16_t ROIRange = sample_rate * 0.6;
int16_t RR_multiplier = 1000 / sample_rate;
long peakSquare;

IntervalTimer sensorTimer;
IntervalTimer flushData;

File rawData;

// -------------------- Define Data Structs --------------------
struct dataBuffers 
{
  //initialise two buffers of 50 each
  int16_t hrdata0[50] = {0};
  int16_t hrmovavg0[50] = {0};
  int16_t hrdata1[50] = {0};
  int16_t hrmovavg1[50] = {0};
  int16_t bufferPointer = 0; //buffer index to write values to
  int16_t bufferMarker = 0; //0 for buffer 0, 1 for buffer 1
  int16_t buffer0State = 0; //0 if clean, 1 if dirty
  int16_t buffer1State = 0;
};

struct workingDataContainer
{
  long absoluteCount = 0;
  
  //buffers
  int16_t curVal = 0;
  int16_t datalen = sample_rate;
  int16_t hrData[sample_rate] = {0};
  int16_t buffPos = 0;
  
  //movavg variables
  int16_t windowSize = sample_rate * 0.6; //windowSize in samples
  int16_t hrMovAvg[sample_rate] = {0};
  int16_t oldestValuePos = 1;
  long movAvgSum = 0;
  int16_t rangeLow = 0;
  int16_t rangeLowNext = 1024;
  int16_t rangeHigh = 1023;
  int16_t rangeHighNext = 1;
  int16_t rangeCounter = 0;
  int16_t rangeRange = 2 * sample_rate;

  //peak variables
  int16_t ROI[ROIRange] = {0};
  //int16_t ROI_interp[40] = {0};
  int16_t ROIPos = 0;
  int16_t peakFlag = 0;
  int8_t ROI_overflow = 0;
  long curPeak = 0;
  long curPeakEnd = 0;
  long lastPeak = 0;
  int8_t peakDet = 0;

  //peak validation variables
  int8_t initFlag = 0; //use for initialisation
  int16_t lastRR = 0;
  int16_t curRR = 0;
  int16_t recent_RR[20] = {0};
  int16_t RR_mean = 0;
  int16_t RR_sum = 0;
  int16_t RR_pos = 0;
  int16_t lower_threshold = 0;
  int16_t upper_threshold = 1;
};

struct dataBuffers dataBuf;
struct workingDataContainer workingData;

// -------------------- Define Helper Functions --------------------
void stopWorking()
{
  digitalWrite(13, HIGH);
  delay(100);
  while(1==1)
  {
    delay(100000);
  }
}

void prepareSD()
{
  Serial.println("starting SD prep");
  if (!SD.begin(BUILTIN_SDCARD)) {
    Serial.println("initialisation failed!");
    stopWorking();
  }
  Serial.println("initialisation done! Continuing...");

  rawData = SD.open("hrdata.csv", FILE_WRITE);
  rawData.print("\n-------------\nNew Measurement\n-------------\n");
  rawData.print("hr\n");
}

int findMax(int16_t arr[], int16_t arrLen, struct workingDataContainer &workingData)
{
  largestVal = 0;
  largestValPos = 0;
  clippingcount = 0;
  clipFlag = 0;
  clipStart = 0;
  clipEnd = 0;
  lastVal = 0;
  
  for(int i = 0; i<arrLen; i++)
  {
    if((abs(lastVal - arr[i]) <= 3))
    {
      if(clipFlag == 0)
      {
        clipFlag = 1;
        clipStart = i;
      } else {
        clippingcount++;
      }
    } else {
      if(clipFlag == 1)
      {
        clipEnd = i;
      }
    }
    
    lastVal = arr[i];    
    
    if(arr[i] > largestVal) 
    {
      largestVal = arr[i];
      largestValPos = i;
    }

    /*if(clippingcount > 3)
    {
      largestValPos = (clipStart + (clipEnd - clipStart)) / 2;
    }*/
  }
    
  return workingData.curPeakEnd - (arrLen - largestValPos);
}

void getMeanRR(struct workingDataContainer &workingData)
{ //returns the mean of the RR_list array.
  workingData.RR_sum = 0;
  for(int i = 0; i<20; i++)
  {
    workingData.RR_sum += workingData.recent_RR[i];
  }
  workingData.RR_mean = workingData.RR_sum / 20;  
}

long mapl(long x, long in_min, long in_max)
{
  return (x - in_min) * 1023 / (in_max - in_min) + 1;
}

void establish_range(struct workingDataContainer &workingData)
{
  if(workingData.rangeCounter <= workingData.rangeRange)
  {
    //update upcoming ranges
    if(workingData.rangeLowNext > workingData.curVal) workingData.rangeLowNext = workingData.curVal;
    if(workingData.rangeHighNext < workingData.curVal) workingData.rangeHighNext = workingData.curVal;
    workingData.rangeCounter++;
  } else {
    //set range, minimum range should be bigger than 50
    //otherwise set to default of (0, 1024)
    if((workingData.rangeHighNext - workingData.rangeLowNext) > 50)
    {
      //update range
      workingData.rangeLow = workingData.rangeLowNext;
      workingData.rangeHigh = workingData.rangeHighNext;
      workingData.rangeLowNext = 1024;
      workingData.rangeHighNext = 1;      
    } else {
      //reset range to default
      workingData.rangeLow = 0;
      workingData.rangeHigh = 1024;
    }
    workingData.rangeCounter = 0;
  }
}

// -------------------- Define Main Functions --------------------
void readSensors(struct workingDataContainer &workingData)
{
  //digitalWrite(13, HIGH);
  workingData.curVal = analogRead(hrpin); //read latest sensor value

  establish_range(workingData);
  workingData.curVal = mapl(workingData.curVal, workingData.rangeLow, workingData.rangeHigh);
  if(workingData.curVal < 0) workingData.curVal = 0;
  //if(workingData.curVal > 1023) workingData.curVal = 1023;

  //peakSquare = workingData.curVal * workingData.curVal;
  //workingData.curVal = mapl(peakSquare, (workingData.rangeLow * workingData.rangeLow), 
  //                          (workingData.rangeHigh * workingData.rangeHigh));
  //if(workingData.curVal < 0) workingData.curVal = 0;

  
  workingData.movAvgSum += workingData.curVal; //update total sum by adding recent value
  workingData.movAvgSum -= workingData.hrData[workingData.oldestValuePos];  //as well as subtracting oldest value
  workingData.hrMovAvg[workingData.buffPos] = workingData.movAvgSum / workingData.windowSize; //compute moving average
  workingData.hrData[workingData.buffPos] = workingData.curVal; //store sensor value
  //digitalWrite(13, LOW);

  //put sensor value in correct buffer
  if (dataBuf.bufferMarker == 0) 
  {
    dataBuf.hrdata0[dataBuf.bufferPointer] = workingData.curVal;
    dataBuf.hrmovavg0[dataBuf.bufferPointer] = workingData.hrMovAvg[workingData.buffPos];
  } else 
  {
    dataBuf.hrdata1[dataBuf.bufferPointer] = workingData.curVal;
    dataBuf.hrmovavg1[dataBuf.bufferPointer] = workingData.hrMovAvg[workingData.buffPos];
  }
  dataBuf.bufferPointer++;
}

void checkForPeak(struct workingDataContainer &workingData)
{
  if(workingData.hrData[workingData.buffPos] >= workingData.hrMovAvg[workingData.buffPos])
  {
    if(workingData.ROIPos >= ROIRange){
      workingData.ROI_overflow = 1;
      return;
    } else {
      workingData.peakFlag = 1;
      workingData.ROI[workingData.ROIPos] = workingData.curVal;
      workingData.ROIPos++;
      workingData.ROI_overflow = 0;
      return;
    }
  }
  
  if((workingData.hrData[workingData.buffPos] <= workingData.hrMovAvg[workingData.buffPos])
  && (workingData.peakFlag == 1))
  {
    if(workingData.ROI_overflow == 1)
    {
      workingData.ROI_overflow = 0;
    } else {
      //solve for peak
      workingData.lastRR = workingData.curRR;
      workingData.curPeakEnd = workingData.absoluteCount;
      workingData.lastPeak = workingData.curPeak;
      workingData.curPeak = findMax(workingData.ROI, workingData.ROIPos, workingData);
      workingData.curRR = (workingData.curPeak - workingData.lastPeak) * RR_multiplier;
      //Serial.println(workingData.curPeak);
      //add peak to struct
    }
    workingData.peakFlag = 0;
    workingData.ROIPos = 0;

    //error detection run, timed at ????????????????????

    if(workingData.curRR > max_RR || workingData.curRR < min_RR)
    {
      return; //break if outside of BPM bounds anyway
    } else if(workingData.initFlag != 0)
    {
      validatePeak(workingData);
    } else {
      updatePeak(workingData);
    }
  }
}

void validatePeak(struct workingDataContainer &workingData)
{
  //validate peaks by thresholding, only update if within thresholded band
  if(workingData.initFlag != 0)
  {
    getMeanRR(workingData);
    if((workingData.RR_mean* 0.3) <= 300){
      workingData.lower_threshold = workingData.RR_mean - 300;
      workingData.upper_threshold = workingData.RR_mean + 300;
    } else{
      workingData.lower_threshold = workingData.RR_mean - (0.3 * workingData.RR_mean);
      workingData.upper_threshold = workingData.RR_mean + (0.3 * workingData.RR_mean);
    }
    
    if(//workingData.curRR < workingData.upper_threshold &&
    //workingData.curRR > workingData.lower_threshold &&
    abs(workingData.curRR - workingData.lastRR) < 500)
    {
      updatePeak(workingData);
    }
  }
}

void updatePeak(struct workingDataContainer &workingData)
{
  //updates peak positions, adds RR-interval to recent intervals
  workingData.recent_RR[workingData.RR_pos] = workingData.curRR;
  workingData.RR_pos++;
  
  if(workingData.RR_pos >= 20)
  {
    workingData.RR_pos = 0;
    workingData.initFlag = 1;
  }
  workingData.peakDet = 1;
    //peakData.printf("%i,%i\n", workingData.curPeak, workingData.curRR);
    //peakData.flush();
}


// -------------------- Define Timer Interrupts --------------------
void interruptFunc()
{ 
  /* define interrupt service routine
  * timed total interrupt routine at max 250 microsec
  * more than fast enough for 100Hz.
  * higher sampling rate not recommended due to increased RAM requirements
  */
  //read the sensor value
  Serial.println(workingData.absoluteCount);
  readSensors(workingData);

  //check if peak is present, update variables if so
  checkForPeak(workingData);
  
  //report raw signal if requested
  /*if(report_hr) 
  {
    rawData.printf("%i,%i\n", workingData.hrMovAvg[workingData.buffPos], workingData.curVal);
  }
  rawData.flush();*/
  //update buffer position pointers
  workingData.buffPos++;
  workingData.oldestValuePos++;

  //reset buffer pointers if at end of buffer
  if(workingData.buffPos >= sample_rate) workingData.buffPos = 0;
  if(workingData.oldestValuePos >= sample_rate) workingData.oldestValuePos = 0;

  //increment total sample counter (used for RR determination)
  workingData.absoluteCount++;
}

// -------------------- Setup --------------------
void setup()
{
  pinMode(13, OUTPUT);
  //start serial
  Serial.begin(250000);
  prepareSD();
  
  //start timer interrupts
  sensorTimer.begin(interruptFunc, (1000000 / sample_rate));
  //flushData.begin(flushSD, (sample_rate * 10000));
}

// -------------------- Main Loop --------------------
void loop()
{
  if(workingData.peakDet == 1)
  {
    rawData.printf("P:%i,%i\n", workingData.curPeak, workingData.curRR);
    workingData.peakDet = 0;
  }
  
  if ((dataBuf.bufferPointer >=  49) && (dataBuf.bufferMarker == 0)) 
  { //time to switch buffer0 to buffer1
    if(dataBuf.buffer1State == 1)  //check if buffer1 is dirty before switching
    {
      Serial.println("buffer0 overflow"); //report error if dirty
      delay(20); //give the processor some time to finish serial print before halting
      exit(0); //halt processor
    } else 
    { //if switching is possible
      dataBuf.buffer0State = 1; //mark buffer0 dirty
    }
    dataBuf.bufferMarker = 1; //set buffer flag to buffer1
    dataBuf.bufferPointer = 0; //reset datapoint bufferPointer
    
    digitalWrite(13, HIGH);
    for (int i = 0; i < 49; i++) { //write contents of buffer0
      rawData.printf("%i,%i\n", dataBuf.hrdata0[i], dataBuf.hrmovavg0[i]);
    }
    digitalWrite(13, LOW);
    rawData.flush();
    Serial.println("written buffer 0");
    
    dataBuf.buffer0State = 0; //release buffer0 after data tranmission, mark as clean
    //here follows same as above, except with reversed buffer order
  } else if ((dataBuf.bufferPointer >= 49) && (dataBuf.bufferMarker == 1)) 
  {
    if(dataBuf.buffer0State == 1)
    {
      Serial.println("buffer1 overflow");
      delay(20);
      exit(0);
    } else 
    {
      dataBuf.buffer1State = 1;
    }
    dataBuf.bufferMarker = 0;
    dataBuf.bufferPointer = 0;
    
    digitalWrite(13, HIGH);
    for (int i = 0; i < 49; i++) 
    {
      rawData.printf("%i,%i\n", dataBuf.hrdata1[i], dataBuf.hrmovavg1[i]);
    }
    digitalWrite(13, LOW);
    rawData.flush();
    Serial.println("written buffer 1");
    
    dataBuf.buffer1State = 0;
  }
}
