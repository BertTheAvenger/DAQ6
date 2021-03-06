#include "Sensor.h"

class SensorDynamicTime : public Sensor
{
public:
    SensorDynamicTime() {};
    ~SensorDynamicTime() {};

    virtual uint16_t readPacketBlock(uint8_t* buffer){
        long time = millis();
        buffer[0] = 0x01;
        buffer[1] = (uint32_t)(time >> 24);
        buffer[2] = (uint32_t)(time >> 16);
        buffer[3] = (uint32_t)(time >> 8);
        buffer[4] = (uint32_t) time;
        sensorPrint("Time: ");
        sensorPrint(time);
        sensorPrint(";\t");
        return 5; //ID byte + 4 byte sensor reading
    } //Writes a packet to the buffer, returns the size of data written.

};