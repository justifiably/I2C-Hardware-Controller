/*  Physical user interface module provides rotary encoders, switches and potentiometer input as I2C device
 *  Copyright riban 2019
 *  Licence LGPL3
 *  Author Brian Walton (brian@riban.co.uk)
 *
 *  Provides ADC with noise filtering and configurable resolution - better noise filtering at lower resolution
 *  Provides switch / button with debounce noise filtering
 *  Provides rotary encoder with switch debounce noise filtering
 *  Provides interrupt signal to master device to indicate controller value has changed
 */

#include <Wire_slave.h>

// ***Adjust these values to suit hardware installation***
#define I2CADDR       0x08 // Set HWC I2C address (must be unique on I2C bus)
#define POTS            64 // Set quantity of end-stopped knobs and faders (0..MAX_POTS)
#define SWITCHES        40 // Set quantity of continuous rotary knobs (0..MAX_SWITCHES)
#define ENCODERS        40 // Set quantity of on / off switches (0..MAX_ENCODERS)
#define ADC_BITS        10 // Set analogue to digital conversion resolution (max 12 bits)
#define ROT_THRESHOLD   10 // Threshold at which to change rotational scaling
#define ROT_FAST_SCALE  10 // Factor to muliptly rotational change rate for fast scrolling
#define DEBOUNCE_TIME   50 // Time in ms to ignore switch value change after previous change
#define USE_ADC_MUX      1 // Set to enable external ADC multiplexers (4051) otherwise use just 8 ADC inputs

// Do not adjust these:
#define MAX_POTS        64 // Maximum quantity of potentiometer controls
#define MAX_SWITCHES    40 // Maximum quantity of on / off switch controls
#define MAX_ENCODERS    40 // Maximum quantity of rotary encoder controls

// Constants
static const uint8_t anValid[] = {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0}; // Table of valid encoder states
//!@todo Optimise GPI allocation for PCB layout
static const uint8_t AnaloguePins[] = {11,10,9,8,7,6,5,4}; // Table of ADC input pins
static const uint8_t InterruptPin = 2; // GPI pin used to indicate values have changed
static const uint8_t MatrixOutputPins[] = {3,17,18,19,20,21,22,23,24,25}; // Table of GPI matrix output pins
static const uint8_t SwitchInputPins[] = {26,27,28,29}; // Table of GPI matrix input pins for switches
static const uint8_t EncoderInputPins[] = {30,31,32,14,13,12,0,1}; // Table of GPI matrix input pins for encoders
static const unsigned int ADC_MASK_BITS = 12 - ADC_BITS; // Quantity of bits to mask ADC
static const int ADC_FILTER_SAMPLES = 32 >> ADC_MASK_BITS; // Quantity of samples to filter ADC
static const uint8_t POT_START = 0; // Offset of potentiometer controls in controller table
static const uint8_t SWITCH_START = MAX_POTS; // Offset of on / off switch controls in controller table
static const uint8_t ENCODER_START = MAX_POTS + MAX_SWITCHES; // Offset of rotary encoder controls in controller table
static const uint8_t MAX_CONTROLLERS = ENCODER_START + MAX_ENCODERS; // Maximum quantity of controllers

/** Controller types */
enum CONTROLLER_TYPE
{
    CONTROLLER_TYPE_POT,
    CONTROLLER_TYPE_SWITCH,
    CONTROLLER_TYPE_ENCODER
};

/** Controller structure represents each type of controller */
struct Controller
{
    bool dirty = false; // True if value has changed since last I2C read
    int value = 0; // Value
    uint8_t count = 0; // Used to filter controller
    uint8_t code = 0; // Used to filter encoder controller
    uint8_t type; // Controller type (see CONTROLLER_TYPE)
    uint32_t time; // Time of last update (ms since boot)
    int getValue() // Get the controller value with any required processing
    {
        int nValue = value;
        switch(type)
        {
        case CONTROLLER_TYPE_POT:
            nValue >>= ADC_MASK_BITS; // Returns value scaled by ADC bits
            break;
        case CONTROLLER_TYPE_ENCODER:
            nValue = value;
            value = 0; // Reset relative position
            break;
        case CONTROLLER_TYPE_SWITCH:
            nValue = value;
        }
        return nValue;
    }
};

// Global variables
uint8_t g_nLastRead = 0; // The index of the controller last read
int g_nI2Cregister = 0; // Address of I2C register requested by I2C master (index of controller - 1 based)
Controller g_anControllers[MAX_CONTROLLERS]; // Array of controller objects

void setup()
{
    // Configure I2C interface
    Wire.begin(I2CADDR);
    Wire.onRequest(onI2Crequest);
    Wire.onReceive(onI2Creceive);

    // Configure GPI pins
    for(int i = 0; i < sizeof(AnaloguePins); ++i)
        pinMode(AnaloguePins[i], INPUT_ANALOG);
    for(int i = 0; i < sizeof(MatrixOutputPins); ++i)
        pinMode(MatrixOutputPins[i], OUTPUT);
    for(int i = 0; i < sizeof(SwitchInputPins); ++i)
        pinMode(SwitchInputPins[i], INPUT_PULLDOWN);
    for(int i = 0; i < sizeof(EncoderInputPins); ++i)
        pinMode(EncoderInputPins[i], INPUT_PULLDOWN);
    pinMode(InterruptPin, OUTPUT);

    // Configure controller objects
    for(int i = POT_START; i < POT_START + MAX_POTS; ++i)
        g_anControllers[i].type = CONTROLLER_TYPE_POT;
    for(int i = SWITCH_START; i < SWITCH_START + MAX_SWITCHES; ++i)
        g_anControllers[i].type = CONTROLLER_TYPE_SWITCH;
    for(int i = ENCODER_START; i < ENCODER_START + MAX_ENCODERS; ++i)
        g_anControllers[i].type = CONTROLLER_TYPE_ENCODER;
}

/** @brief  Read ADCs
*/
void readAdc()
{
    //Monitor analogue inputs (potentiometers)
    for(uint8_t nMux = 0; nMux < 8; ++nMux)
    {
        #ifdef USE_ADC_MUX
        // Configure external analouge multiplexer
        digitalWrite(MatrixOutputPins[0], nMux & 0x01);
        digitalWrite(MatrixOutputPins[1], nMux & 0x02);
        digitalWrite(MatrixOutputPins[2], nMux & 0x04);
        #endif // USE_ADC_MUX
        for(uint8_t nAdc = 0; nAdc < 8; ++nAdc)
        {
            #ifdef USE_ADC_MUX
            uint8_t nPot = nMux + nAdc * 8;
            #else
            uint8_t nPot = nMux * 8 + nAdc;
            #endif // USE_ADC_MUX
            if(nPot < POTS)
            {
                int nAnaRead = analogRead(AnaloguePins[nAdc]);
                if(abs(nAnaRead - g_anControllers[POT_START + nPot].value) < (1 << ADC_MASK_BITS))
                {
                    ++g_anControllers[POT_START + nPot].count = 0;
                    continue;
                }
                if(++g_anControllers[POT_START + nPot].count < ADC_FILTER_SAMPLES)
                    continue;
                ++g_anControllers[POT_START + nPot].count = 0;
                g_anControllers[POT_START + nPot].value = nAnaRead;
                g_anControllers[POT_START + nPot].dirty = true;
                g_anControllers[POT_START + nPot].time = millis();
            }
        }
    }
}

/** @brief  Read on / off switches
*/
void readSwitch()
{
    for(uint8_t nOutput = 0; nOutput < sizeof(MatrixOutputPins); ++nOutput)
        digitalWrite(MatrixOutputPins[nOutput], LOW);
    for(uint8_t nY = 0; nY < sizeof(MatrixOutputPins); ++nY)
    {
        // This is the iteration for setting outupt of Y-axis of switch matrix
        digitalWrite(MatrixOutputPins[nY], HIGH);
        for(uint8_t nX = 0; nX < sizeof(SwitchInputPins); ++nX)
        {
            // This is the iteration for reading the X-axis of switch matrix
            int nSwitch = nY * sizeof(SwitchInputPins) + nX;
            if(nSwitch < SWITCHES)
            {
                int nValue = digitalRead(SwitchInputPins[nX]);
                if(nValue != g_anControllers[SWITCH_START + nSwitch].value && millis() > g_anControllers[SWITCH_START + nSwitch].time + DEBOUNCE_TIME)
                {
                    //  Note: Initial testing suggests that debounce is not required but it is added to handle noisy switches
                    g_anControllers[SWITCH_START + nSwitch].value = nValue;
                    g_anControllers[SWITCH_START + nSwitch].dirty = true;
                    g_anControllers[SWITCH_START + nSwitch].time = millis();
                }
            } else {
                digitalWrite(MatrixOutputPins[nY], LOW);
                return;
            }
        }
        digitalWrite(MatrixOutputPins[nY], LOW);
    }
}

/** @brief  Read rotary encoders
*/
void readEncoder()
{
    int8_t nDir = 0; // Direction of rotation [-1, 0, +1]
    for(uint8_t nOutput = 0; nOutput < sizeof(MatrixOutputPins); ++nOutput)
        digitalWrite(MatrixOutputPins[nOutput], LOW);
    for(uint8_t nY = 0; nY < sizeof(MatrixOutputPins); ++nY)
    {
        // This is the iteration for setting outupt of Y-axis of switch matrix
        digitalWrite(MatrixOutputPins[nY], HIGH);
        for(uint8_t nX = 0; nX < sizeof(EncoderInputPins); nX += 2)
        {
            // This is the iteration for reading the X-axis of encoder matrix
            int nEncoder = (nY * sizeof(EncoderInputPins) + nX) / 2;
            if(nEncoder < ENCODERS)
            {
                int nClk = digitalRead(EncoderInputPins[nX]);
                if(!nClk && !g_anControllers[ENCODER_START + nEncoder].code)
                    continue; // Ignore encoder if clock not asserted or not mid-decode
                int nData = digitalRead(EncoderInputPins[nX + 1]);
                g_anControllers[ENCODER_START + nEncoder].code <<= 2;
                if(nData)
                    g_anControllers[ENCODER_START + nEncoder].code |= 0x02;
                if(nClk)
                    g_anControllers[ENCODER_START + nEncoder].code |= 0x01;
                g_anControllers[ENCODER_START + nEncoder].code &= 0x0f;
                // If valid then add to 8-bit history validate rotation codes and process
                if(anValid[g_anControllers[ENCODER_START + nEncoder].code])
                {
                    g_anControllers[ENCODER_START + nEncoder].count <<= 4;
                    g_anControllers[ENCODER_START + nEncoder].count |= g_anControllers[ENCODER_START + nEncoder].code;
                    if(g_anControllers[ENCODER_START + nEncoder].count == 0xd4)
                        nDir = -1;
                    else if(g_anControllers[ENCODER_START + nEncoder].count == 0x17)
                        nDir = +1;
                    if(nDir)
                    {
                        uint32_t lNow = millis();
                        if(lNow >= g_anControllers[ENCODER_START + nEncoder].time + ROT_THRESHOLD)
                            g_anControllers[ENCODER_START + nEncoder].value += nDir; //Slow rotation
                        else
                            g_anControllers[ENCODER_START + nEncoder].value += nDir * ROT_FAST_SCALE; //Fast rotation
                        g_anControllers[ENCODER_START + nEncoder].time = lNow;
                        g_anControllers[ENCODER_START + nEncoder].code = 0;
                        g_anControllers[ENCODER_START + nEncoder].dirty = true;
                    }
                }
            } else {
                digitalWrite(MatrixOutputPins[nY], LOW);
                return;
            }
        }
        digitalWrite(MatrixOutputPins[nY], LOW);
    }
}

void loop()
{
    readAdc(); // Updates the potentiometers
    readSwitch(); // Update the on / off switches
    readEncoder(); // Update the rotary encoders
    getDirty(); // Check for any value changes since last read or processing cycle
}

/** @brief  Get the next controller who's values has changed since last I2C request
 *  @retval uint8_t Index of controller (1 based) or 0 for none
 *  @note   Avoids one controller hogging bus by starting scan after last read controller
 */
uint8_t getDirty()
{
    // Iterate through all controllers looking for changed, starting at controller after last read then wrapping round
    for(uint8_t i = g_nLastRead; i < MAX_CONTROLLERS; ++i)
    {
        if(g_anControllers[i].dirty)
        {
            digitalWrite(InterruptPin, LOW);
            return(i + 1);
        }
    }
    for(uint8_t i = 0; i < g_nLastRead; ++i)
    {
        if(g_anControllers[i].dirty)
        {
            digitalWrite(InterruptPin, LOW);
            return(i + 1);
        }
    }
    digitalWrite(InterruptPin, HIGH);
    return 0;
}

/** @brief  Sends controller 16-bit value to I2C master
 *  @param  controller Controller index (one based)
 */
void sendValue(uint8_t controller)
{
    if(controller == 0 || controller-- > MAX_CONTROLLERS)
        return;
    int nValue = g_anControllers[controller].getValue();
    uint8_t anValue[] = {nValue & 0xFF, nValue >> 8};
    Wire.flush();
    Wire.write(anValue, 2);
    g_anControllers[controller].dirty = false;
    getDirty(); // Update dirty status incase this was last changed controller
}

/** @brief  Handle I2C request for data from master
 *  @note   Send the requested register (controller) value or index of next changed controller
 */
void onI2Crequest()
{
    if(g_nI2Cregister)
    {
        // Send value of requested controller
        sendValue(g_nI2Cregister);
        g_nI2Cregister = 0;
        return;
    }
    // Send index of first changed controller or zero if all clean
    Wire.flush(); // Seem to need to flush I2C to ensure single byte of data is sent correctly
    Wire.write(getDirty());
}

/** @brief  Handle I2C message from master
 *  @param  nBytes Quantity of bytes in message
 *  @note   Expects single byte containing the register address for next I2C read operation
 */
void onI2Creceive(int nBytes)
{
    if(nBytes < 0)
        return;
    g_nI2Cregister = Wire.read();
    g_nLastRead = g_nI2Cregister;
    while(Wire.available())
        Wire.read(); // Clear buffer as only expecting single byte
}
