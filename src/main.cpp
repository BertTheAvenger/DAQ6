#include <SdFat.h>
//https://github.com/sdesalas/Arduino-Queue.h
#include <queue.h>
#include "config.h"
#include "util.h"
#include "macros.h"
#include "LED.h"

/*Sensors*/
#include "Sensor.h"
#include "SensorTime.h"
#include "SensorMarker.h"
#include "SensorBrakePressure.h"
#include "SensorRotSpeeds.h"
#include "DebouncedButton.h"


/*ChibiOS*/
#include <ChRt.h>
MUTEX_DECL(QueueMod_Mtx);

//The queues hold pointers to the starting addresses of each block of size BLOCK_SIZE.
//This is done to optimize performance and simplify later code.
//These pointers, once initialized, will basically go back and forth between
//the emptyQueue and the writeQueue
Queue<uint8_t*> writeQueue = Queue<uint8_t*>(BLOCK_COUNT);
Queue<uint8_t*> emptyQueue = Queue<uint8_t*>(BLOCK_COUNT);

//Holds the blocks accessed through pointers in the write/empty queue
uint8_t blockBuf[BUFFER_SIZE];

Sensor* sensors[SENSOR_ARR_SIZE];
uint16_t numSensors = 0;


uint8_t packetBuf[MAX_PACKET_SIZE];

LED statusLed(PIN_STATUS_LED);

const uint32_t PREALLOCATE_SIZE_MiB = 2UL;
const uint64_t PREALLOCATE_SIZE  =  (uint64_t)PREALLOCATE_SIZE_MiB << 20;

volatile bool isLogging = false;


/*Func defs*/
void fsmWriter();


void queueBuf(uint8_t* buf, uint16_t size) {
    static uint8_t* currentBlock = emptyQueue.pop(); //On init, get an empty block.
    static uint32_t currentInd = 0;

    for(int i = 0; i < size; i++) {
        //Add a val to the current block...
        currentBlock[currentInd] = buf[i];
        currentInd++;

        //If we have filled the block, push it to the write queue and get an empty one.
        if(currentInd >= BLOCK_SIZE) {
            //Serial.println("Block filled, getting new.");

            //If there's an empty block we can write to, pop it make it ours owo.
            if(emptyQueue.count() > 0) {
                writeQueue.push(currentBlock);
                currentBlock = emptyQueue.pop();
            } else {
                Serial.println("BUFFER OVERRAN!");
                SysCall::halt();
            }

            //Reset the block index.
            currentInd = 0;
        }

    }
}


THD_WORKING_AREA(readerWa, 512);
THD_FUNCTION(reader, arg) {
    static systime_t nextRead = chVTGetSystemTime();
    while(true) {
        if(isLogging) {
            uint16_t size = BufferPacket(packetBuf, sensors, numSensors);
            debug("\n");
            chMtxLock(&QueueMod_Mtx);
            queueBuf(packetBuf, size);
            chMtxUnlock(&QueueMod_Mtx);
        }
        nextRead += TIME_MS2I(SAMPLE_INTERVAL);
        chThdSleepUntil(nextRead);
    }
    
}


void chMain() {
    Serial.println("Starting!");
    chThdCreateStatic(readerWa, sizeof(readerWa), READER_PRIORITY, reader, NULL);

    while(true) {

        fsmWriter();
        statusLed.tick();
    }
}

void setup() {
    Serial.begin(115200);
    while(!Serial){;}
    Serial.println("Starting...");

    /*Sensors*/
    #ifdef SENSOR_TIME 
        sensors[numSensors++] = new SensorTime();
    #endif
    
    #ifdef SENSOR_ECVT 
        Serial.println("ECVT not yet implmented..");
    #endif
    
    #ifdef SENSOR_MARKER 
        sensors[numSensors++] = new SensorMarker(PIN_MARKER_BTN);
    #endif
    
    #ifdef SENSOR_BRAKEPRESSURE 
        sensors[numSensors++] = new SensorBrakePressures(PIN_BPRESSURE_F, PIN_BPRESSURE_R);
    #endif
    
    #ifdef SENSOR_ROTATIONSPEEDS 
        static uint16_t index = numSensors++;
        sensors[index] = new SensorRotSpeeds();
        
        auto engineISR = [](){ ((SensorRotSpeeds*) sensors[index])-> calcESpeed(); };
        auto rWheelISR = [](){ ((SensorRotSpeeds*) sensors[index])->calcRWheels(); };
        auto lFrontISR = [](){ ((SensorRotSpeeds*) sensors[index])->calcFLWheel(); };
        auto rFrontISR = [](){ ((SensorRotSpeeds*) sensors[index])->calcFRWheel(); };

        attachInterrupt(digitalPinToInterrupt(PIN_RSPEED_ENG), engineISR, RISING);
        attachInterrupt(digitalPinToInterrupt(PIN_RSPEED_RGO), rWheelISR, RISING);
        attachInterrupt(digitalPinToInterrupt(PIN_RSPEED_WFL), lFrontISR, RISING);
        attachInterrupt(digitalPinToInterrupt(PIN_RSPEED_WFR), rFrontISR, RISING);
    #endif

    Serial.print(numSensors);
    Serial.println(" sensors initialized!");

    //Populate the empty queue with pointers to each 512 byte-long buffer.
    for(uint32_t i = 0; i < BLOCK_COUNT; i++) {
        uint8_t* bufIndex = &blockBuf[i * BLOCK_SIZE]; //Pointer generated to be at the start of each block.
        emptyQueue.push(bufIndex);
    }

    statusLed.setState(LED::CONSTANT_BLINK);


    chBegin(chMain);
}



void loop() {

}



void fsmWriter() {
    //Positive states are OK states
    static const int STATE_WAITING_TO_START = 0; 
    static const int STATE_STARTING = 1; 
    static const int STATE_WAITING_TO_STOP = 2; 
    static const int STATE_WRITING = 3; 
    static const int STATE_STOPPING = 4; 

    //Neative states are error states
    static const int STATE_SD_INIT_ERR = -1;



    static DebouncedButton loggerBtn(PIN_LOGGER_BTN);

    static int state = -100; //Initialization State

    static SD_TYPE sd;
    static FILE_TYPE file;

    static LED writerLed(PIN_LOGGER_LED);
    
    if(state == -100) {
        /*Writer initial setup*/
        if(sd.begin(254)) {
            state = STATE_STARTING;
        } else {
            Serial.println("SD INTIALIZATION ERROR - RUNNING IN SD-LESS MODE!");
            sd.printSdError(&Serial);
            state = STATE_SD_INIT_ERR;
            writerLed.setState(LED::SOS_BLINK);
        }
        
    }



    switch (state)
        {
        case STATE_WAITING_TO_START: //WAITING TO START
            SET_STATE_IF(loggerBtn.isTriggered(), STATE_STARTING);
            break;



        case STATE_STARTING: {
            char fileName[FILENAME_SIZE];
            SelectNextFilename( fileName , &sd);
            
            //open the file
            SET_STATE_EXEC_IF(!file.open(fileName, O_RDWR | O_CREAT), STATE_WAITING_TO_START, {Serial.println("Error Opening File!");} );

            //preallocate storage.
            if(!file.preAllocate(PREALLOCATE_SIZE)) {
                Serial.println("Preallocation Err.");
                state = STATE_WAITING_TO_START;
                break;
            }

            Serial.print(F("preAllocated: "));
            Serial.print(PREALLOCATE_SIZE_MiB);
            Serial.println(F(" MiB"));
            
            writerLed.setState(LED::ON);
            isLogging = true;
            state = STATE_WAITING_TO_STOP;
            break;
        } 



        case STATE_WAITING_TO_STOP: {
            SET_STATE_IF(!sd.card()->isBusy() && writeQueue.count() > 0, STATE_WRITING);  //If card not busy and data availible, write.
            SET_STATE_IF(loggerBtn.isTriggered(), STATE_STOPPING);
            break;
        }

        case STATE_WRITING: {
            //Pop a block pointer from the write queue, and write it to SD. 
            //Prevent anything from touching the queue while this happens through the mtx.
            chMtxLock(&QueueMod_Mtx);
            uint8_t* poppedBlock = writeQueue.pop();
            chMtxUnlock(&QueueMod_Mtx);

            size_t written = file.write(poppedBlock, BLOCK_SIZE); //Can do this w/o checking block B/C if we're here, data is guarenteed to be in queue.
            if(written != BLOCK_SIZE) {Serial.println("Write Failed! :(");}
            
            //Once done, put the written back on the empty queue to be used again.
            chMtxLock(&QueueMod_Mtx);
            emptyQueue.push(poppedBlock);
            chMtxUnlock(&QueueMod_Mtx);

            
            state = STATE_WAITING_TO_STOP;
            break;
        }

        case STATE_STOPPING: {
            isLogging = false;
            file.truncate();
            file.sync();
            file.close();
            Serial.println("Stopping logging!");
            writerLed.setState(LED::OFF);
            state = STATE_WAITING_TO_START;
        }
    }

    writerLed.tick();

}



