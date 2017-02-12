/*
  Code written by Thomas Carpenter 2012-2017
  
  With thanks Chris over at the EQMOD Yahoo group for assisting decoding the Skywatcher protocol
  
  
  Equatorial mount tracking system for integration with EQMOD using the Skywatcher/Synta
  communication protocol.
 
  Works with EQ5, HEQ5, and EQ6 mounts, and also a great many custom mount configurations.
 
  Current Verison: 7.5.1
*/

//Only works with ATmega162, and Arduino Mega boards (1280 and 2560)
#if defined(__AVR_ATmega162__) || defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)


/*
 * Include headers
 */
 
#include "AstroEQ.h" //AstroEQ header

#include "EEPROMReader.h" //Read config file
#include "SerialLink.h" //Serial Port
#include "UnionHelpers.h" //Union prototypes
#include "synta.h" //Synta Communications Protocol.
#include <util/delay.h>    
#include <util/delay_basic.h>
#include <avr/wdt.h>

// Watchdog disable on boot.
void wdt_init(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdt_init(void)
{
    wdt_disable();
    return;
}

/*
 * Defines
 */
//Define the version number
#define ASTROEQ_VER 751
//Define the welcome string for the advanced handcontroller. This is the version number as hex.
#define SPI_WELCOME_STRING "=4B\r"

/*
 * Global Variables
 */
byte stepIncrement[2];
byte readyToGo[2] = {0,0};
unsigned long gotoPosn[2] = {0UL,0UL}; //where to slew to
bool encodeDirection[2];
byte progMode = RUNMODE; //MODES:  0 = Normal Ops (EQMOD). 1 = Validate EEPROM. 2 = Store to EEPROM. 3 = Rebuild EEPROM
byte microstepConf;
byte driverVersion;

#define timerCountRate 8000000

#define DecimalDistnWidth 32
unsigned int timerOVF[2][DecimalDistnWidth];
bool canJumpToHighspeed = false;
bool defaultSpeedState = SPEEDNORM;
bool disableGearChange = false;
bool allowAdvancedHCDetection = false;
unsigned int gotoDecelerationLength[2];
byte accelTableRepeatsLeft[2] = {0,0};
byte accelTableIndex[2] = {0,0};

/*
 * Helper Macros
 */
#define distributionSegment(m)      (m ? GPIOR1     : GPIOR2    )
#define currentMotorSpeed(m)        (m ? OCR3A      : OCR3B     )
#define irqToNextStep(m)            (m ? OCR1A      : OCR1B     )
#define interruptOVFCount(m)        (m ? ICR3       : ICR1      )
#define interruptControlRegister(m) (m ? TIMSK3     : TIMSK1    )
#define interruptControlBitMask(m)  (m ? _BV(ICIE3) : _BV(ICIE1))
#define timerCountRegister(m)       (m ? TCNT3      : TCNT1     )
#define timerPrescalarRegister(m)   (m ? TCCR3B     : TCCR1B    )
#define gotoDeceleratingBitMask(m)  (m ? _BV(3)     : _BV(2)    )
#define gotoRunningBitMask(m)       (m ? _BV(1)     : _BV(0)    )
#define gotoControlRegister GPIOR0




/*
 * Inline functions
 */
inline bool gotoRunning(const byte axis) {
    return (gotoControlRegister & gotoRunningBitMask(axis));
}
inline bool gotoDecelerating(const byte axis) {
    return (gotoControlRegister & gotoDeceleratingBitMask(axis));
}
inline void setGotoRunning(const byte axis) {
    gotoControlRegister |= gotoRunningBitMask(axis);
}
inline void clearGotoRunning(const byte axis) {
    gotoControlRegister &= ~gotoRunningBitMask(axis);
}
inline void setGotoDecelerating(const byte axis) {
    gotoControlRegister |= gotoDeceleratingBitMask(axis);
}
inline void clearGotoDecelerating(const byte axis) {
    gotoControlRegister &= ~gotoDeceleratingBitMask(axis);
}



/*
 * Generate Mode Mappings
 */

#define MODE0 0
#define MODE1 1
#define MODE2 2
#define MODE0DIR 3
#define MODE1DIR 4
#define MODE2DIR 5
byte modeState[2] = {((LOW << MODE2) | (HIGH << MODE1) | (HIGH << MODE0)), (( LOW << MODE2) | ( LOW << MODE1) | (LOW << MODE0))}; //Default to 1/8th stepping as that is the same for all

void buildModeMapping(byte microsteps, byte driverVersion){
    //For microstep modes less than 8, we cannot jump to high speed, so we use the SPEEDFAST mode maps. Given that the SPEEDFAST maps are generated for the microstepping modes >=8
    //anyway, we can simply multiply the number of microsteps by 8 if it is less than 8 and thus reduce the number of cases in the mode generation switch statement below 
    if (microsteps < 8){
        microsteps *= 8;
    }
    //Generate the mode mapping for the current driver version and microstepping modes.
    switch (microsteps) {
        case 8:
            // 1/8
            modeState[SPEEDNORM] =                                                                                       (( LOW << MODE2) | (HIGH << MODE1) | (HIGH << MODE0));
            // 1/1
            modeState[SPEEDFAST] =                                                                                       (( LOW << MODE2) | ( LOW << MODE1) | ( LOW << MODE0));
            break;
        case 32:
            // 1/32
            modeState[SPEEDNORM] = (driverVersion == DRV8834) ? ((FLOAT << MODE2) | (HIGH << MODE1) | ( LOW << MODE0)) : ((HIGH << MODE2) | (HIGH << MODE1) | (HIGH << MODE0));
            // 1/4
            modeState[SPEEDFAST] = (driverVersion == DRV8834) ? ((FLOAT << MODE2) | ( LOW << MODE1) | ( LOW << MODE0)) : (( LOW << MODE2) | (HIGH << MODE1) | ( LOW << MODE0));
            break;
        case 16:
        default:  //Unknown. Default to half/sixteenth stepping
            // 1/16
            modeState[SPEEDNORM] = (driverVersion == DRV882x) ? ((  LOW << MODE2) | ( LOW << MODE1) | (HIGH << MODE0)) : ((HIGH << MODE2) | (HIGH << MODE1) | (HIGH << MODE0));
            // 1/2
            modeState[SPEEDFAST] = (driverVersion == DRV882x) ? (( HIGH << MODE2) | ( LOW << MODE1) | ( LOW << MODE0)) : (( LOW << MODE2) | ( LOW << MODE1) | (HIGH << MODE0));
            break;
    }
}




/*
 * System Initialisation Routines
 */

void calculateDecelerationLength (byte axis){

    unsigned int gotoSpeed = cmd.normalGotoSpeed[axis];
    byte lookupTableIndex = 0;
    unsigned int numberOfSteps = 0;
    //Work through the acceleration table until we get to the right speed (accel and decel are same number of steps)
    while(lookupTableIndex < AccelTableLength) {
        if (cmd.accelTable[axis][lookupTableIndex].speed <= gotoSpeed) {
            //If we have reached the element at which we are now at the right speed
            break; //We have calculated the number of accel steps and therefore number of decel steps.
        }
        numberOfSteps = numberOfSteps + cmd.accelTable[axis][lookupTableIndex].repeats + 1; //Add on the number of steps at this speed (1 step + number of repeats)
        lookupTableIndex++;
    }
    //number of steps now contains how many steps required to slow to a stop.
    gotoDecelerationLength[axis] = numberOfSteps;
}

void calculateRate(byte axis){
  
    unsigned long rate;
    unsigned long remainder;
    float floatRemainder;
    unsigned long divisor = cmd.bVal[axis];
    byte distWidth = DecimalDistnWidth;
    
    //When dividing a very large number by a much smaller on, float accuracy is abismal. So firstly we use integer math to split the division into quotient and remainder
    rate = timerCountRate / divisor; //Calculate the quotient
    remainder = timerCountRate % divisor; //Calculate the remainder
    
    //Then convert the remainder into a decimal number (division of a small number by a larger one, improving accuracy)
    floatRemainder = (float)remainder/(float)divisor; //Convert the remainder to a decimal.
    
    //Multiply the remainder by distributionWidth to work out an approximate number of extra clocks needed per full step (each step is 'distributionWidth' microsteps)
    floatRemainder *= (float)distWidth; 
    //This many extra cycles are needed:
    remainder = (unsigned long)(floatRemainder+0.5f); 
    
    //Now truncate to an unsigned int with a sensible max value (the int is to avoid register issues with the 16 bit timer)
    if((unsigned int)(rate >> 16)){
        rate = 65535UL;
    } else if (rate < 128UL) {
        rate = 128UL;
    }
#if defined(__AVR_ATmega162__)
    rate--;
#endif
  
    for (byte i = 0; i < distWidth; i++){
#if defined(__AVR_ATmega162__)
        timerOVF[axis][i] = rate; //Subtract 1 as timer is 0 indexed.
#else
        timerOVF[axis][i] = rate; //Hmm, for some reason this one doesn't need 1 subtracting???
#endif
    }
  
    //evenly distribute the required number of extra clocks over the full step.
    for (unsigned long i = 0; i < remainder; i++){
        float distn = i;
        distn *= (float)distWidth;
        distn /= (float)remainder;
        byte index = (byte)ceil(distn);
        timerOVF[axis][index] += 1;
    }
    
}

void systemInitialiser(){    
    
    encodeDirection[RA] = EEPROM_readByte(RAReverse_Address) ? CMD_REVERSE : CMD_FORWARD;  //reverse the right ascension if 1
    encodeDirection[DC] = EEPROM_readByte(DECReverse_Address) ? CMD_REVERSE : CMD_FORWARD; //reverse the declination if 1
    
    driverVersion = EEPROM_readByte(Driver_Address);
    microstepConf = EEPROM_readByte(Microstep_Address);

    allowAdvancedHCDetection = !EEPROM_readByte(AdvHCEnable_Address);
    
    defaultSpeedState = (microstepConf >= 8) ? SPEEDNORM : SPEEDFAST;
    disableGearChange = !EEPROM_readByte(GearEnable_Address);
    canJumpToHighspeed = (microstepConf >= 8) && !disableGearChange; //Gear change is enabled if the microstep mode can change by a factor of 8.
        
    synta_initialise(ASTROEQ_VER,(canJumpToHighspeed ? 8 : 1)); //initialise mount instance, specify version!
    
    buildModeMapping(microstepConf, driverVersion);
    
    if(!checkEEPROM()){
        progMode = PROGMODE; //prevent AstroEQ startup if EEPROM is blank.
    }

    calculateRate(RA); //Initialise the interrupt speed table. This now only has to be done once at the beginning.
    calculateRate(DC); //Initialise the interrupt speed table. This now only has to be done once at the beginning.
    calculateDecelerationLength(RA);
    calculateDecelerationLength(DC);
    
    //Status pin to output low
    setPinDir  (statusPin,OUTPUT);
    setPinValue(statusPin,   LOW);

    //Standalone Speed/IRQ pin to input no-pullup
    setPinDir  (standalonePin[  STANDALONE_IRQ], INPUT);
    setPinValue(standalonePin[  STANDALONE_IRQ],   LOW);

    //Standalone Pullup/Pulldown pin to output high
    setPinDir  (standalonePin[ STANDALONE_PULL],OUTPUT);
    setPinValue(standalonePin[ STANDALONE_PULL],  HIGH);
    
    //ST4 pins to input with pullup
    setPinDir  (st4Pin[RA][ST4P],INPUT);
    setPinValue(st4Pin[RA][ST4P],HIGH );
    setPinDir  (st4Pin[RA][ST4N],INPUT);
    setPinValue(st4Pin[RA][ST4N],HIGH );
    setPinDir  (st4Pin[DC][ST4P],INPUT);
    setPinValue(st4Pin[DC][ST4P],HIGH );
    setPinDir  (st4Pin[DC][ST4N],INPUT);
    setPinValue(st4Pin[DC][ST4N],HIGH );
    
    //Reset pins to output
    setPinDir  (resetPin[RA],OUTPUT);
    setPinValue(resetPin[RA],   LOW);  //Motor driver in Reset
    setPinDir  (resetPin[DC],OUTPUT);
    setPinValue(resetPin[DC],   LOW);  //Motor driver in Reset 
    
    //Enable pins to output
    setPinDir  (enablePin[RA],OUTPUT);
    setPinValue(enablePin[RA],  HIGH); //Motor Driver Disabled
    setPinDir  (enablePin[DC],OUTPUT);
    setPinValue(enablePin[DC],  HIGH); //Motor Driver Disabled
    
    //Step pins to output
    setPinDir  (stepPin[RA],OUTPUT);
    setPinValue(stepPin[RA],   LOW);
    setPinDir  (stepPin[DC],OUTPUT);
    setPinValue(stepPin[DC],   LOW);
    
    //Direction pins to output
    setPinDir  (dirPin[RA],OUTPUT);
    setPinValue(dirPin[RA],   LOW);
    setPinDir  (dirPin[DC],OUTPUT);
    setPinValue(dirPin[DC],   LOW);
    
    //Load the correct mode
    byte state = modeState[defaultSpeedState]; //Extract the default mode - If the microstep mode is >= then we start in NORMAL mode, otherwise we use FAST mode

    setPinValue(modePins[RA][MODE0], (state & (byte)(1<<MODE0   )));
    setPinDir  (modePins[RA][MODE0],  OUTPUT                      ); 
    setPinValue(modePins[DC][MODE0], (state & (byte)(1<<MODE0   )));
    setPinDir  (modePins[DC][MODE0],  OUTPUT                      );
    setPinValue(modePins[RA][MODE1], (state & (byte)(1<<MODE1   )));
    setPinDir  (modePins[RA][MODE1],  OUTPUT                      );
    setPinValue(modePins[DC][MODE1], (state & (byte)(1<<MODE1   )));
    setPinDir  (modePins[DC][MODE1],  OUTPUT                      );
    setPinValue(modePins[RA][MODE2], (state & (byte)(1<<MODE2   )));
    setPinDir  (modePins[RA][MODE2],!(state & (byte)(1<<MODE2DIR))); //For the DRV8834 type, we also need to set the direction of the Mode2 bit to be an input if floating is required for this step mode.
    setPinValue(modePins[DC][MODE2], (state & (byte)(1<<MODE2   )));
    setPinDir  (modePins[DC][MODE2],!(state & (byte)(1<<MODE2DIR))); //For the DRV8834 type, we also need to set the direction of the Mode2 bit to be an input if floating is required for this step mode.

    //Give some time for the Motor Drivers to reset.
    _delay_ms(1);

    //Then bring them out of reset.
    setPinValue(resetPin[RA],HIGH);
    setPinValue(resetPin[DC],HIGH);
    
#if defined(__AVR_ATmega162__)
    //Disable Timer 0
    //Timer 0 registers are being used as general purpose data storage for high efficency
    //interrupt routines. So timer must be fully disabled. The ATMegaxxx0 has three of these
    //registers, but the ATMega162 doesn't, so I've had to improvise and use other registers
    //instead. See PinMappings.h for the ATMega162 to see which registers have been #defined
    //as GPIORx.
    TIMSK &= ~(_BV(TOIE0) | _BV(OCIE0));
    TCCR0 = 0;
#endif

    //Ensure SPI is disabled
    SPI_disable();
    
    //Initialise the Serial port:
    Serial_initialise(BAUD_RATE); //SyncScan runs at 9600Baud, use a serial port of your choice as defined in SerialLink.h
      
    //Configure interrupt for ST4 connection
#if defined(__AVR_ATmega162__)
    //For ATMega162:
    PCICR &= ~_BV(PCIE1); //disable PCInt[8..15] vector
    PCICR |=  _BV(PCIE0); //enable  PCInt[0..7]  vector
#elif defined(ALTERNATE_ST4)
    //For ATMega1280/2560 with Alterante ST4 pins
    PCICR |=  _BV(PCIE2); //enable  PCInt[16..23] vector
    PCICR &= ~_BV(PCIE1); //disable PCInt[8..15]  vector
    PCICR &= ~_BV(PCIE0); //disable PCInt[0..7]   vector
#else
    //For ATMega1280/2560 with Standard ST4 pins
    PCICR &= ~_BV(PCIE2); //disable PCInt[16..23] vector
    PCICR &= ~_BV(PCIE1); //disable PCInt[8..15]  vector
    PCICR |=  _BV(PCIE0); //enable  PCInt[0..7]   vector
#endif
}




/*
 * EEPROM Validation and Programming Routines
 */

bool checkEEPROM(){
    char temp[9] = {0};
    EEPROM_readString(temp,8,AstroEQID_Address);
    if(strncmp(temp,"AstroEQ",8)){
        return false;
    }
    if (driverVersion > DRV8834){
        return false; //invalid value.
    }
    if ((driverVersion == A498x) && microstepConf > 16){
        return false; //invalid value.
    } else if (microstepConf > 32){
        return false; //invalid value.
    }
    if ((cmd.siderealIVal[RA] > 1200) || (cmd.siderealIVal[RA] < MIN_IVAL)) {
        return false; //invalid value.
    }
    if ((cmd.siderealIVal[DC] > 1200) || (cmd.siderealIVal[DC] < MIN_IVAL)) {
        return false; //invalid value.
    }
    if(cmd.normalGotoSpeed[RA] == 0){
        return false; //invalid value.
    }
    if(cmd.normalGotoSpeed[DC] == 0){
        return false; //invalid value.
    }
    return true;
}

void buildEEPROM(){
    EEPROM_writeString("AstroEQ",8,AstroEQID_Address);
}

void storeEEPROM(){
    EEPROM_writeLong(cmd.aVal[RA],aVal1_Address);
    EEPROM_writeLong(cmd.aVal[DC],aVal2_Address);
    EEPROM_writeLong(cmd.bVal[RA],bVal1_Address);
    EEPROM_writeLong(cmd.bVal[DC],bVal2_Address);
    EEPROM_writeLong(cmd.sVal[RA],sVal1_Address);
    EEPROM_writeLong(cmd.sVal[DC],sVal2_Address);
    EEPROM_writeByte(encodeDirection[RA],RAReverse_Address);
    EEPROM_writeByte(encodeDirection[DC],DECReverse_Address);
    EEPROM_writeByte(driverVersion,Driver_Address);
    EEPROM_writeByte(microstepConf,Microstep_Address);
    EEPROM_writeByte(cmd.normalGotoSpeed[RA],RAGoto_Address);
    EEPROM_writeByte(cmd.normalGotoSpeed[DC],DECGoto_Address);
    EEPROM_writeInt(cmd.siderealIVal[RA],IVal1_Address);
    EEPROM_writeInt(cmd.siderealIVal[DC],IVal2_Address);
    EEPROM_writeByte(!disableGearChange, GearEnable_Address);
    EEPROM_writeByte(!allowAdvancedHCDetection, AdvHCEnable_Address);
    EEPROM_writeAccelTable(cmd.accelTable[RA],AccelTableLength,AccelTable1_Address);
    EEPROM_writeAccelTable(cmd.accelTable[DC],AccelTableLength,AccelTable2_Address);
}









/*
 * Standalone Helpers
 */

byte standaloneModeTest() {
    if (allowAdvancedHCDetection) {
        //Don't allow advanced detection if user has disabled (i.e. no detection hardware implemented).
        setPinValue(standalonePin[STANDALONE_IRQ],HIGH); //enable pull-up to pull IRQ high.
    } else {
        //We need to test what sort of controller is attached.
        //The IRQ pin on the ST4 connector is used to determine this. It has the following
        //states:
        //   FLOAT      | No handcontroller
        //   DRIVE LOW  | Basic handcontroller
        //   DRIVE HIGH | Advanced handcontroller
        //We can test for each of these states by virtue of having a controllable pull up/down
        //resistor on that pin.
        //If we pull down and the pin stays high, then pin must be driven high (DRIVE HIGH)
        //If we pull up and the pin stays low, then pin must be driven low (DRIVE LOW)
        //Otherwise if pin follows us then it must be floating.
    
        //To start we check for an advanced controller
        setPinValue(standalonePin[STANDALONE_PULL],LOW); //Pull low
        nop(); // Input synchroniser takes a couple of cycles
        nop();
        nop();
        nop();
        if(getPinValue(standalonePin[STANDALONE_IRQ])) {
            //Must be an advanced controller as pin stayed high.
            return ADVANCED_HC_MODE;
        }
        setPinValue(standalonePin[STANDALONE_PULL],HIGH); //Convert to external pull-up of IRQ
    }
    //Otherwise we check for a basic controller
    nop(); // Input synchroniser takes a couple of cycles
    nop();
    nop();
    nop();
    if(!getPinValue(standalonePin[STANDALONE_IRQ])) {
        //Must be a basic controller as pin stayed low.
        return BASIC_HC_MODE;
    }


    //If we get this far then it is floating, so we assume EQMOD mode
    return EQMOD_MODE;
}



/*
 * AstroEQ firmware main() function
 */

int main(void) {
    //Enable global interrupt flag
    sei();
    //Initialise global variables from the EEPROM
    systemInitialiser();
    
    bool standaloneMode = false; //Initially not in standalone mode (EQMOD mode)
    bool syntaMode = true; //And synta processing is enabled.
    bool hcFastSpeed = false; //Also not in basic hand controller fast speed mode.
    bool mcuReset = false; //Not resetting the MCU after programming command
    
    unsigned int loopCount = 0;
    char recievedChar = 0; //last character we received
    int8_t decoded = 0; //Whether we have decoded the packet
    char decodedPacket[11]; //temporary store for completed command ready to be processed
    
    for(;;){ //Run loop

        loopCount++; //Counter used to time events based on number of loops.

        if (!standaloneMode && (loopCount == 0)) { 
            //If we are not in standalone mode, periodically check if we have just entered it
            byte mode = standaloneModeTest();
            if (mode != EQMOD_MODE) {
                //If we have just entered stand-alone mode, then we enable the motors and configure the mount
                motorStop(RA, true); //Ensure both motors are stopped
                motorStop(DC, true);
                
                //This next bit needs to be atomic
                byte oldSREG = SREG; 
                cli();  
                cmd_setjVal(RA, 0x800000); //set the current position to the middle
                cmd_setjVal(DC, 0x800000); //set the current position to the middle
                SREG = oldSREG;
                //End atomic
                //Disable Serial
                Serial_disable();
    
                //We are now in standalone mode.
                standaloneMode = true; 
                
                //Next check what type of hand controller we have
                if (mode == ADVANCED_HC_MODE) {
                    //We pulled low, but pin stayed high
                    //This means we must have an advanced controller actively pulling the line high
                    syntaMode = true; 
                    
                    //Disable interrupts for ST4 connection as this is no longer being used for ST4
    #if defined(__AVR_ATmega162__)
                    //For ATMega162:
                    PCMSK0 =  0x00; //PCINT[4..7]
    #elif defined(ALTERNATE_ST4)
                    //For ATMega1280/2560 with Alterante ST4 pins
                    PCMSK2 =  0x00; //PCINT[16..23]
    #else
                    //For ATMega1280/2560 with Standard ST4 pins
                    PCMSK0 =  0x00; //PCINT[0..3]
    #endif
                    //Initialise SPI for advanced comms
                    SPI_initialise();
    
                    //And send welcome message
                    Serial_writeStr(SPI_WELCOME_STRING); //Send version number
                    
                } else {
                    //Pin either is being pulled low by us or by something else
                    //This means we might have a basic controller actively pulling the line low
                    //Even if we don't we would default to basic mode.
                    syntaMode = false;
                    
                    //For basic mode we need a pull up resistor on the speed/irq line
                    setPinValue(standalonePin[STANDALONE_PULL],HIGH); //Pull high
    
                    //And then we need to initialise the controller manually so the basic controller can help us move
                    byte state = modeState[defaultSpeedState]; //Extract the default mode - for basic HC we won't change from default mode.
                    setPinValue(modePins[RA][MODE0], (state & (byte)(1<<MODE0   )));
                    setPinValue(modePins[DC][MODE0], (state & (byte)(1<<MODE0   )));
                    setPinValue(modePins[RA][MODE1], (state & (byte)(1<<MODE1   )));
                    setPinValue(modePins[DC][MODE1], (state & (byte)(1<<MODE1   )));
                    setPinValue(modePins[RA][MODE2], (state & (byte)(1<<MODE2   )));
                    setPinValue(modePins[DC][MODE2], (state & (byte)(1<<MODE2   )));
                    
                    hcFastSpeed = false; //Assume we are not in highspeed mode at the moment.
                    Commands_configureST4Speed(CMD_ST4_STANDALONE); //Change the ST4 speeds
                    
                    motorEnable(RA); //Ensure the motors are enabled
                    motorEnable(DC);
                    cmd_setGVal(RA, 1); //Set both axes to slew mode.
                    cmd_setGVal(DC, 1);
                    cmd_setDir (RA, CMD_FORWARD); //Store the current direction for that axis
                    cmd_setDir (DC, CMD_FORWARD); //Store the current direction for that axis
                    cmd_setIVal(RA, cmd.siderealIVal[RA]); //Set RA speed to sidereal
                    readyToGo[RA] = 1; //Signal we are ready to go on the RA axis to start sideral tracking
                }
            }
            //If we end up in standalone mode, we don't exit until a reset.
        }
        
        if (syntaMode) { //EQMOD or Advanced Hand Controller Synta Mode
            
            //Check if we need to run the command parser
            
            if ((decoded == -2) || Serial_available()) { //is there a byte in buffer or we still need to process the previous byte?
                //Toggle on the LED to indicate activity.
                togglePin(statusPin);
                //See what character we need to parse
                if (decoded != -2) {
                    //get the next character in buffer
                    recievedChar = Serial_read(); 
                } //otherwise we will try to parse the previous character again.
                //Append the current character and try to parse the command
                decoded = synta_recieveCommand(decodedPacket,recievedChar); 
                //Once full command packet recieved, synta_recieveCommand populates either an error packet (and returns -1), or data packet (returns 1). If incomplete, decodedPacket is unchanged and 0 is returned
                if (decoded != 0){ //Send a response
                    if (decoded > 0){ //Valid Packet, current command is in decoded variable.
                        mcuReset = !decodeCommand(decoded,decodedPacket); //decode the valid packet and populate response.
                    }
                    Serial_writeStr(decodedPacket); //send the response packet (recieveCommand() generated the error packet, or decodeCommand() a valid response)
                } //otherwise command not yet fully recieved, so wait for next byte
                
                if (mcuReset) {
                    exit(0); //Special case. We were asked to reset the MCU. The WDT has been set to reset.
                }
            }
        } else { //ST4 Basic Hand Controller Mode
            
            //Parse the stand-alone mode speed

            if (loopCount == 0) {
                togglePin(statusPin); //Toggle status pin at roughly constant rate in basic mode as indicator
            }
            
            if (getPinValue(standalonePin[1])) {
                //If in normal speed mode
                if (hcFastSpeed) {
                    //But we have just changed from highspeed
                    Commands_configureST4Speed(CMD_ST4_STANDALONE); //Change the ST4 speeds
                    hcFastSpeed = false;
                }
            } else {
                //If in high speed mode
                if (!hcFastSpeed) {
                    //But we have just changed from normal speed
                    Commands_configureST4Speed(CMD_ST4_HIGHSPEED); //Change the ST4 speeds
                    hcFastSpeed = true;
                }
            }
        }
    
    
        //Check both axes - loop unravelled for speed efficiency - lots of Flash available.
        if(readyToGo[RA]==1){
            //If we are ready to begin a movement which requires the motors to be reconfigured
            if((cmd.stopped[RA])){
                //Once the motor is stopped, we can accelerate to target speed.
                signed char GVal = cmd.GVal[RA];
                if (canJumpToHighspeed){
                    //If we are allowed to enable high speed, see if we need to
                    byte state;
                    if ((GVal == 1) || (GVal == 2)) {
                        //If a low speed mode command
                        state = modeState[SPEEDNORM]; //Select the normal speed mode
                        cmd_updateStepDir(RA,1);
                        cmd.highSpeedMode[RA] = false;
                    } else {
                        state = modeState[SPEEDFAST]; //Select the high speed mode
                        cmd_updateStepDir(RA,cmd.gVal[RA]);
                        cmd.highSpeedMode[RA] = true;
                    }
                    setPinValue(modePins[RA][MODE0], (state & (byte)(1<<MODE0)));
                    setPinValue(modePins[RA][MODE1], (state & (byte)(1<<MODE1)));
                    setPinValue(modePins[RA][MODE2], (state & (byte)(1<<MODE2)));
                } else {
                    //Otherwise we never need to change the speed
                    cmd_updateStepDir(RA,1); //Just move along at one step per step
                    cmd.highSpeedMode[RA] = false;
                }
                if(GVal & 1){
                    //This is the funtion that enables a slew type move.
                    slewMode(RA); //Slew type
                    readyToGo[RA] = 2;
                } else {
                    //This is the function for goto mode. You may need to customise it for a different motor driver
                    gotoMode(RA); //Goto Mode
                    readyToGo[RA] = 0;
                }
            } //Otherwise don't start the next movement until we have stopped.
        }
        if(readyToGo[DC]==1){
            //If we are ready to begin a movement which requires the motors to be reconfigured
            if((cmd.stopped[DC])){
                //Once the motor is stopped, we can accelerate to target speed.
                signed char GVal = cmd.GVal[DC];
                if (canJumpToHighspeed){
                    //If we are allowed to enable high speed, see if we need to
                    byte state;
                    if ((GVal == 1) || (GVal == 2)) {
                        //If a low speed mode command
                        state = modeState[SPEEDNORM]; //Select the normal speed mode
                        cmd_updateStepDir(DC,1);
                        cmd.highSpeedMode[DC] = false;
                    } else {
                        state = modeState[SPEEDFAST]; //Select the high speed mode
                        cmd_updateStepDir(DC,cmd.gVal[DC]);
                        cmd.highSpeedMode[DC] = true;
                    }
                    setPinValue(modePins[DC][MODE0], (state & (byte)(1<<MODE0)));
                    setPinValue(modePins[DC][MODE1], (state & (byte)(1<<MODE1)));
                    setPinValue(modePins[DC][MODE2], (state & (byte)(1<<MODE2)));
                } else {
                    //Otherwise we never need to change the speed
                    cmd_updateStepDir(DC,1); //Just move along at one step per step
                    cmd.highSpeedMode[DC] = false;
                }
                if(GVal & 1){
                    //This is the funtion that enables a slew type move.
                    slewMode(DC); //Slew type
                    readyToGo[DC] = 2; //We are now in a running mode which speed can be changed without stopping motor (unless a command changes the direction)
                } else {
                    //This is the function for goto mode.
                    gotoMode(DC); //Goto Mode
                    readyToGo[DC] = 0; //We are now in a mode where no further changes can be made to the motor (apart from requesting a stop) until the go-to movement is done.
                }
            } //Otherwise don't start the next movement until we have stopped.
        }
    }//End of run loop
}




/*
 * Decode and Perform the Command
 */

bool decodeCommand(char command, char* buffer){ //each command is axis specific. The axis being modified can be retrieved by calling synta_axis()
    unsigned long responseData = 0; //data for response
    byte axis = synta_axis();
    unsigned int correction;
    byte oldSREG;
    switch(command) {
        case 'e': //readonly, return the eVal (version number)
            responseData = cmd.eVal[axis]; //response to the e command is stored in the eVal function for that axis.
            break;
        case 'a': //readonly, return the aVal (steps per axis)
            responseData = cmd.aVal[axis]; //response to the a command is stored in the aVal function for that axis.
            break;
        case 'b': //readonly, return the bVal (sidereal step rate)
            responseData = cmd.bVal[axis]; //response to the b command is stored in the bVal function for that axis.
            if (!progMode) {
                //If not in programming mode, we need to apply a correction factor to ensure that calculations in EQMOD round correctly
                correction = (cmd.siderealIVal[axis] << 1);
                responseData = (responseData * (correction+1))/correction; //account for rounding inside Skywatcher DLL.
            }
            break;
        case 'g': //readonly, return the gVal (high speed multiplier)
            responseData = cmd.gVal[axis]; //response to the g command is stored in the gVal function for that axis.
            break;
        case 's': //readonly, return the sVal (steps per worm rotation)
            responseData = cmd.sVal[axis]; //response to the s command is stored in the sVal function for that axis.
            break;
        case 'f': //readonly, return the fVal (axis status)
            responseData = cmd_fVal(axis); //response to the f command is stored in the fVal function for that axis.
            break;
        case 'j': //readonly, return the jVal (current position)
            oldSREG = SREG; 
            cli();  //The next bit needs to be atomic, just in case the motors are running
            responseData = cmd.jVal[axis]; //response to the j command is stored in the jVal function for that axis.
            SREG = oldSREG;
            break;
        case 'K': //stop the motor, return empty response
            motorStop(axis,0); //normal ISR based decelleration trigger.
            readyToGo[axis] = 0;
            break;
        case 'L':
            motorStop(axis,1); //emergency axis stop.
            motorDisable(axis); //shutdown driver power.
            break;
        case 'G': //set mode and direction, return empty response
            /*if (packetIn[0] == '0'){
              packetIn[0] = '2'; //don't allow a high torque goto. But do allow a high torque slew.
            }*/
            cmd_setGVal(axis, (buffer[0] - '0')); //Store the current mode for the axis
            cmd_setDir(axis, (buffer[1] != '0') ? CMD_REVERSE : CMD_FORWARD); //Store the current direction for that axis
            readyToGo[axis] = 0;
            break;
        case 'H': //set goto position, return empty response (this sets the number of steps to move from cuurent position if in goto mode)
            cmd_setHVal(axis, synta_hexToLong(buffer)); //set the goto position container (convert string to long first)
            readyToGo[axis] = 0;
            break;
        case 'I': //set slew speed, return empty response (this sets the speed to move at if in slew mode)
            responseData = synta_hexToLong(buffer); //convert string to long first
            if (responseData < cmd.accelTable[axis][AccelTableLength-1].speed) {
                //Limit the IVal to the largest speed in the acceleration table to prevent sudden rapid acceleration at the end.
                responseData = cmd.accelTable[axis][AccelTableLength-1].speed; 
            }
            cmd_setIVal(axis, responseData); //set the speed container
            responseData = 0;
            if (readyToGo[axis] == 2) {
                //If we are in a running mode which allows speed update without motor reconfiguration
                motorStart(axis); //Simply update the speed.
            } else {
                //Otherwise we are no longer ready to go until the next :J command is received
                readyToGo[axis] = 0;
            }
            break;
        case 'E': //set the current position, return empty response
            oldSREG = SREG; 
            cli();  //The next bit needs to be atomic, just in case the motors are running
            cmd_setjVal(axis, synta_hexToLong(buffer)); //set the current position (used to sync to what EQMOD thinks is the current position at startup
            SREG = oldSREG;
            break;
        case 'F': //Enable the motor driver, return empty response
            if (progMode == 0) { //only allow motors to be enabled outside of programming mode.
                motorEnable(axis); //This enables the motors - gives the motor driver board power
            } else {
                command = 0; //force sending of error packet!.
            }
            break;
            
            
        //The following are used for configuration ----------
        case 'A': //store the aVal (steps per axis)
            cmd_setaVal(axis, synta_hexToLong(buffer)); //store aVal for that axis.
            break;
        case 'B': //store the bVal (sidereal rate)
            cmd_setbVal(axis, synta_hexToLong(buffer)); //store bVal for that axis.
            break;
        case 'S': //store the sVal (steps per worm rotation)
            cmd_setsVal(axis, synta_hexToLong(buffer)); //store sVal for that axis.
            break;
        case 'n': //return the IVal (EQMOD Speed at sidereal)
            responseData = cmd.siderealIVal[axis];
            break;
        case 'N': //store the IVal (EQMOD Speed at sidereal)
            cmd_setsideIVal(axis, synta_hexToLong(buffer)); //store sVal for that axis.
            break;
        case 'd': //return the driver version or step mode
            if (axis) {
                responseData = microstepConf; 
            } else {
                responseData = driverVersion;
            }
            break;
        case 'D': //store the driver verison and step modes
            if (axis) {
                microstepConf = synta_hexToByte(buffer); //store step mode.
                canJumpToHighspeed = (microstepConf >=8);
            } else {
                driverVersion = synta_hexToByte(buffer); //store driver version.
            }
            break;
        case 'z': //return the Goto speed
            responseData = cmd.normalGotoSpeed[axis];
            break;
        case 'Z': //return the Goto speed factor
            cmd.normalGotoSpeed[axis] = synta_hexToByte(buffer); //store the goto speed factor
            break;
        case 'c': //return the axisDirectionReverse
            responseData = encodeDirection[axis];
            break;
        case 'C': //store the axisDirectionReverse
            encodeDirection[axis] = buffer[0] - '0'; //store sVal for that axis.
            break;
        case 'q': //return the disableGearChange/allowAdvancedHCDetection setting  
            if (axis) {
                responseData = disableGearChange; 
            } else {
                responseData = allowAdvancedHCDetection;
            }
            break;
        case 'Q': //store the disableGearChange/allowAdvancedHCDetection setting
            if (axis) {
                disableGearChange = synta_hexToByte(buffer); //store whether we can change gear
            } else {
                allowAdvancedHCDetection = synta_hexToByte(buffer); //store whether to allow advanced hand controller detection
            }
            break;
        case 'x': {  //return the accelTable
            Inter responsePack = InterMaker(0);
            responsePack.lowByter.integer = cmd.accelTable[axis][accelTableIndex[axis]].speed;
            responsePack.highByter.low = cmd.accelTable[axis][accelTableIndex[axis]].repeats; 
            responseData = responsePack.integer;
            accelTableIndex[axis]++; //increment the index so we don't have to send :Y commands for every address if reading sequentially.
            if (accelTableIndex[axis] >= AccelTableLength) {
                accelTableIndex[axis] = 0; //Wrap around
            }
            break;
        }
        case 'X': { //store the accelTable value for address set by 'Y', or next address after last 'X'
            unsigned long dataIn = synta_hexToLong(buffer);
            cmd.accelTable[axis][accelTableIndex[axis]].speed = (unsigned int)dataIn; //lower two bytes is speed
            cmd.accelTable[axis][accelTableIndex[axis]].repeats = (byte)(dataIn>>16); //upper byte is repeats.
            accelTableIndex[axis]++; //increment the index so we don't have to send :Y commands for every address if programming sequentially.
            if (accelTableIndex[axis] >= AccelTableLength) {
                accelTableIndex[axis] = 0; //Wrap around
            }
            break;
        }
        case 'Y': //store the accelTableIndex value
            //Use axis=0 to set which address we are accessing (we'll repurpose accelTableIndex[RA] in prog mode for this)
            accelTableIndex[axis] = synta_hexToByte(buffer);
            if (accelTableIndex[axis] >= AccelTableLength) {
                command = '\0'; //If the address out of range, force an error response packet.
            }
            break;
        case 'O': //set the programming mode.
            progMode = buffer[0] - '0';              //MODES:  0 = Normal Ops (EQMOD). 1 = Validate EEPROM. 2 = Store to EEPROM. 3 = Rebuild EEPROM
            if (progMode != 0) {
                motorStop(RA,1); //emergency axis stop.
                motorDisable(RA); //shutdown driver power.
                motorStop(DC,1); //emergency axis stop.
                motorDisable(DC); //shutdown driver power.
                readyToGo[RA] = 0;
                readyToGo[DC] = 0;
            }
            break;
        case 'T': //set mode, return empty response
            if (progMode & 2) {
            //proceed with eeprom write
                if (progMode & 1) {
                    buildEEPROM();
                } else {
                    storeEEPROM();
                }
            } else if (progMode & 1) {
                if (!checkEEPROM()) { //check if EEPROM contains valid data.
                    command = 0; //force sending of an error packet.
                }
            }
            break;
        //---------------------------------------------------
        default: //Return empty response (deals with commands that don't do anything before the response sent (i.e 'J', 'R'), or do nothing at all (e.g. 'M') )
            break;
    }
  
    synta_assembleResponse(buffer, command, responseData); //generate correct response (this is required as is)
    
    if (command == 'R') { //reset the uC
        wdt_enable(WDTO_120MS);
        return false;
    }
    if ((command == 'J') && (progMode == 0)) { //J tells us we are ready to begin the requested movement.
        readyToGo[axis] = 1; //So signal we are ready to go and when the last movement complets this one will execute.
        if (!(cmd.GVal[axis] & 1)){
            //If go-to mode requested
            cmd_setGotoEn(axis,CMD_ENABLED);
        }
    }
    return true;
}










void motorEnable(byte axis){
    if (axis == RA){
        setPinValue(enablePin[RA],LOW); //IC enabled
        cmd_setFVal(RA,CMD_ENABLED);
    } else {
        setPinValue(enablePin[DC],LOW); //IC enabled
        cmd_setFVal(DC,CMD_ENABLED);
    }
    configureTimer(); //setup the motor pulse timers.
}

void motorDisable(byte axis){
    if (axis == RA){
        setPinValue(enablePin[RA],HIGH); //IC enabled
        cmd_setFVal(RA,CMD_DISABLED);
    } else {
        setPinValue(enablePin[DC],HIGH); //IC enabled
        cmd_setFVal(DC,CMD_DISABLED);
    }
}

void slewMode(byte axis){
    motorStart(axis); //Begin PWM
}

void gotoMode(byte axis){
    unsigned int decelerationLength = gotoDecelerationLength[axis];
    
    if (cmd.highSpeedMode[axis]) {
        //Additionally in order to maintain the same speed profile in high-speed mode, we actually increase the profile repeats by a factor of sqrt(8)
        //compared with running in normal-speed mode. See Atmel AVR466 app note for calculation.
        decelerationLength = decelerationLength * 3; //multiply by 3 as it is approx sqrt(8)
    }
    
    byte dirMagnitude = abs(cmd.stepDir[axis]);
    byte dir = cmd.dir[axis];

    if (cmd.HVal[axis] < 2*dirMagnitude){
        cmd_setHVal(axis,2*dirMagnitude);
    }

    decelerationLength = decelerationLength * dirMagnitude;
    //decelleration length is here a multiple of stepDir.
    unsigned long HVal = cmd.HVal[axis];
    unsigned long halfHVal = (HVal >> 1);
    unsigned int gotoSpeed = cmd.normalGotoSpeed[axis];
    if(dirMagnitude == 8){
        HVal &= 0xFFFFFFF8; //clear the lower bits to avoid overshoot.
    }
    if(dirMagnitude == 8){
        halfHVal &= 0xFFFFFFF8; //clear the lower bits to avoid overshoot.
    }
    //HVal and halfHVal are here a multiple of stepDir
    if (halfHVal < decelerationLength) {
        decelerationLength = halfHVal;
    }
    HVal -= decelerationLength;
    gotoPosn[axis] = cmd.jVal[axis] + ((dir == CMD_REVERSE) ? -HVal : HVal); //current position + relative change - decelleration region
    
    cmd_setIVal(axis, gotoSpeed);
    clearGotoDecelerating(axis);
    setGotoRunning(axis); //start the goto.
    motorStart(axis); //Begin PWM
}

inline void timerEnable(byte motor) {
    timerPrescalarRegister(motor) &= ~((1<<CSn2) | (1<<CSn1));//00x
    timerPrescalarRegister(motor) |= (1<<CSn0);//xx1
}

inline void timerDisable(byte motor) {
    interruptControlRegister(motor) &= ~interruptControlBitMask(motor); //Disable timer interrupt
    timerPrescalarRegister(motor) &= ~((1<<CSn2) | (1<<CSn1) | (1<<CSn0));//00x
}

//As there is plenty of FLASH left, then to improve speed, I have created two motorStart functions (one for RA and one for DEC)
void motorStart(byte motor){
    if (motor == RA) {
        motorStartRA();
    } else {
        motorStartDC();
    }
}
void motorStartRA(){
    unsigned int IVal = cmd.IVal[RA];
    unsigned int currentIVal;
    unsigned int startSpeed;
    unsigned int stoppingSpeed;
    
    interruptControlRegister(RA) &= ~interruptControlBitMask(RA); //Disable timer interrupt
    currentIVal = currentMotorSpeed(RA);
    interruptControlRegister(RA) |= interruptControlBitMask(RA); //enable timer interrupt
    
    if (IVal > cmd.minSpeed[RA]){
        stoppingSpeed = IVal;
    } else {
        stoppingSpeed = cmd.minSpeed[RA];
    }
    if(cmd.stopped[RA]) {
        startSpeed = stoppingSpeed;
    } else if (currentIVal < cmd.minSpeed[RA]) {
        startSpeed = currentIVal;
    } else {
        startSpeed = stoppingSpeed;
    }
    
    interruptControlRegister(RA) &= ~interruptControlBitMask(RA); //Disable timer interrupt
    cmd.currentIVal[RA] = cmd.IVal[RA];
    currentMotorSpeed(RA) = startSpeed;
    cmd.stopSpeed[RA] = stoppingSpeed;
    setPinValue(dirPin[RA],(encodeDirection[RA] != cmd.dir[RA]));
    
    if(cmd.stopped[RA]) { //if stopped, configure timers
        irqToNextStep(RA) = 1;
        accelTableRepeatsLeft[RA] = cmd.accelTable[RA][0].repeats; //If we are stopped, we must do the required number of repeats for the first entry in the speed table.
        accelTableIndex[RA] = 0;
        distributionSegment(RA) = 0;
        timerCountRegister(RA) = 0;
        interruptOVFCount(RA) = timerOVF[RA][0];
        timerEnable(RA);
        cmd_setStopped(RA, CMD_RUNNING);
    }
    interruptControlRegister(RA) |= interruptControlBitMask(RA); //enable timer interrupt
}

void motorStartDC(){
    unsigned int IVal = cmd.IVal[DC];
    unsigned int currentIVal;
    interruptControlRegister(DC) &= ~interruptControlBitMask(DC); //Disable timer interrupt
    currentIVal = currentMotorSpeed(DC);
    interruptControlRegister(DC) |= interruptControlBitMask(DC); //enable timer interrupt
    
    unsigned int startSpeed;
    unsigned int stoppingSpeed;
    if (IVal > cmd.minSpeed[DC]){
        stoppingSpeed = IVal;
    } else {
        stoppingSpeed = cmd.minSpeed[DC];
    }
    if(cmd.stopped[DC]) {
        startSpeed = stoppingSpeed;
    } else if (currentIVal < cmd.minSpeed[DC]) {
        startSpeed = currentIVal;
    } else {
        startSpeed = stoppingSpeed;
    }
    
    interruptControlRegister(DC) &= ~interruptControlBitMask(DC); //Disable timer interrupt
    cmd.currentIVal[DC] = cmd.IVal[DC];
    currentMotorSpeed(DC) = startSpeed;
    cmd.stopSpeed[DC] = stoppingSpeed;
    setPinValue(dirPin[DC],(encodeDirection[DC] != cmd.dir[DC]));
    
    if(cmd.stopped[DC]) { //if stopped, configure timers
        irqToNextStep(DC) = 1;
        accelTableRepeatsLeft[DC] = cmd.accelTable[DC][0].repeats; //If we are stopped, we must do the required number of repeats for the first entry in the speed table.
        accelTableIndex[DC] = 0;
        distributionSegment(DC) = 0;
        timerCountRegister(DC) = 0;
        interruptOVFCount(DC) = timerOVF[DC][0];
        timerEnable(DC);
        cmd_setStopped(DC, CMD_RUNNING);
    }
    interruptControlRegister(DC) |= interruptControlBitMask(DC); //enable timer interrupt
}

//As there is plenty of FLASH left, then to improve speed, I have created two motorStop functions (one for RA and one for DEC)
void motorStop(byte motor, byte emergency){
    if (motor == RA) {
        motorStopRA(emergency);
    } else {
        motorStopDC(emergency);
    }
}

void motorStopRA(byte emergency){
    if (emergency) {
        //trigger instant shutdown of the motor in an emergency.
        timerDisable(RA);
        cmd_setGotoEn(RA,CMD_DISABLED); //Not in goto mode.
        cmd_setStopped(RA,CMD_STOPPED); //mark as stopped
        cmd_setGVal(RA, 0); //Switch back to slew mode (in case we just finished a GoTo)
        readyToGo[RA] = 0;
        clearGotoRunning(RA);
    } else if (!cmd.stopped[RA]){  //Only stop if not already stopped - for some reason EQMOD stops both axis when slewing, even if one isn't currently moving?
        //trigger ISR based decelleration
        //readyToGo[RA] = 0;
        cmd_setGotoEn(RA,CMD_DISABLED); //No longer in goto mode.
        clearGotoRunning(RA);
        cmd_setGVal(RA, 0); //Switch back to slew mode (in case we just finished a GoTo)
        interruptControlRegister(RA) &= ~interruptControlBitMask(RA); //Disable timer interrupt
        if(cmd.currentIVal[RA] < cmd.minSpeed[RA]){
            if(cmd.stopSpeed[RA] > cmd.minSpeed[RA]){
                cmd.stopSpeed[RA] = cmd.minSpeed[RA];
            }
        }/* else {
            stopSpeed[RA] = cmd.currentIVal[RA];
        }*/
        cmd.currentIVal[RA] = cmd.stopSpeed[RA] + 1;//cmd.stepIncrement[motor];
        interruptControlRegister(RA) |= interruptControlBitMask(RA); //enable timer interrupt
    }
}

void motorStopDC(byte emergency){
    if (emergency) {
        //trigger instant shutdown of the motor in an emergency.
        timerDisable(DC);
        cmd_setGotoEn(DC,CMD_DISABLED); //Not in goto mode.
        cmd_setStopped(DC,CMD_STOPPED); //mark as stopped
        cmd_setGVal(DC, 0); //Switch back to slew mode (in case we just finished a GoTo)
        readyToGo[DC] = 0;
        clearGotoRunning(DC);
    } else if (!cmd.stopped[DC]){  //Only stop if not already stopped - for some reason EQMOD stops both axis when slewing, even if one isn't currently moving?
        //trigger ISR based decelleration
        //readyToGo[motor] = 0;
        cmd_setGotoEn(DC,CMD_DISABLED); //No longer in goto mode.
        cmd_setGVal(DC, 0); //Switch back to slew mode (in case we just finished a GoTo)
        clearGotoRunning(DC);
        interruptControlRegister(DC) &= ~interruptControlBitMask(DC); //Disable timer interrupt
        if(cmd.currentIVal[DC] < cmd.minSpeed[DC]){
            if(cmd.stopSpeed[DC] > cmd.minSpeed[DC]){
                cmd.stopSpeed[DC] = cmd.minSpeed[DC];
            }
        }/* else {
        stopSpeed[DC] = cmd.currentIVal[DC];
        }*/
        cmd.currentIVal[DC] = cmd.stopSpeed[DC] + 1;//cmd.stepIncrement[motor];
        interruptControlRegister(DC) |= interruptControlBitMask(DC); //enable timer interrupt
    }
}

//Timer Interrupt-----------------------------------------------------------------------------
void configureTimer(){
    interruptControlRegister(DC) = 0; //disable all timer interrupts.
#if defined(__AVR_ATmega162__)
    interruptControlRegister(RA) &= 0b00000011; //for 162, the lower 2 bits of the declination register control another timer, so leave them alone.
#else
    interruptControlRegister(RA) = 0;
#endif
    //set to ctc mode (0100)
    TCCR1A = 0;//~((1<<WGM11) | (1<<WGM10));
    TCCR1B = ((1<<WGM12) | (1<<WGM13));
    TCCR3A = 0;//~((1<<WGM31) | (1<<WGM30));
    TCCR3B = ((1<<WGM32) | (1<<WGM33));
  
}

#ifdef ALTERNATE_ST4
ISR(PCINT2_vect)
#else
ISR(PCINT0_vect)
#endif
{
    //ST4 Pin Change Interrupt Handler.
    if(!cmd.gotoEn[RA] && !cmd.gotoEn[DC]){
        //Only allow when not it goto mode.
        {//Start RA
            byte dir;
            byte stepDir;
            bool stopped = (cmd.stopped[RA] == CMD_STOPPED) || (cmd.st4RAReverse == CMD_REVERSE);
            unsigned int newSpeed;
            if (cmd.dir[RA] && !stopped) {
                goto ignoreRAST4; //if travelling in the wrong direction and not allowing reverse, then ignore.
            }
            if (!getPinValue(st4Pin[RA][ST4N])) {
                //RA-
                if (cmd.st4RAReverse == CMD_REVERSE) {
                    dir = CMD_REVERSE;
                    stepDir = -1;
                } else {
                    dir = CMD_FORWARD;
                    stepDir = 1;
                }
                newSpeed = cmd.st4RAIVal[1]; //------------ 0.75x sidereal rate
                goto setRASpeed;
            } else if (!getPinValue(st4Pin[RA][ST4P])) {
                //RA+
                dir = CMD_FORWARD;
                stepDir = 1;
                newSpeed = cmd.st4RAIVal[0]; //------------ 1.25x sidereal rate
setRASpeed:
                cmd.currentIVal[RA] = newSpeed;
                if (stopped) {
                    cmd.stepDir[RA] = stepDir; //set step direction
                    cmd.dir[RA] = dir; //set direction
                    cmd.GVal[RA] = 1; //slew mode
                    motorStartRA();
                } else if (cmd.stopSpeed[RA] < cmd.currentIVal[RA]) {
                    cmd.stopSpeed[RA] = cmd.currentIVal[RA]; //ensure that RA doesn't stop.
                }
            } else {
ignoreRAST4:
                dir = CMD_FORWARD;
                stepDir = 1;
                newSpeed = cmd.siderealIVal[RA];
                goto setRASpeed;
            }
        }//End RA

        {//Start DEC
            byte dir;
            byte stepDir;
            if (!getPinValue(st4Pin[DC][ST4N])) {
                //DEC-
                dir = CMD_REVERSE;
                stepDir = -1;
                goto setDECSpeed;
            } else if (!getPinValue(st4Pin[DC][ST4P])) {
                //DEC+
                dir = CMD_FORWARD;
                stepDir = 1;
setDECSpeed:
                cmd.stepDir[DC] = stepDir; //set step direction
                cmd.dir[DC] = dir; //set direction
                cmd.currentIVal[DC] = cmd.st4DecIVal; //move at 0.25x sidereal rate
                cmd.GVal[DC] = 1; //slew mode
                motorStartDC();
            } else {
                cmd.currentIVal[DC] = cmd.stopSpeed[DC] + 1;//make our target >stopSpeed so that ISRs bring us to a halt.
            }
        }//End DEC
    }
}




/*Timer Interrupt Vector*/
ISR(TIMER3_CAPT_vect) {
    
    //Load the number of interrupts until the next step
    unsigned int irqToNext = irqToNextStep(DC)-1;
    //Check if we are ready to step
    if (irqToNext == 0) {
        //Once the required number of interrupts have occurred...
        
        //First update the interrupt base rate using our distribution array. 
        //This affords a more accurate sidereal rate by dithering the intterrupt rate to get higher resolution.
        byte timeSegment = distributionSegment(DC); //Get the current time segement
        
        /* 
        byte index = ((DecimalDistnWidth-1) & timeSegment) >> 1; //Convert time segment to array index
        interruptOVFCount(DC) = timerOVF[DC][index]; //Update interrupt base rate.
        */// Below is optimised version of above:
        byte index = ((DecimalDistnWidth-1) << 1) & timeSegment; //Convert time segment to array index
        interruptOVFCount(DC) = *(int*)((byte*)timerOVF[DC] + index); //Update interrupt base rate.
        
        distributionSegment(DC) = timeSegment + 1; //Increment time segement for next time.

        unsigned int currentSpeed = currentMotorSpeed(DC); //Get the current motor speed
        irqToNextStep(DC) = currentSpeed; //Update interrupts to next step to be the current speed in case it changed (accel/decel)
        
        if (getPinValue(stepPin[DC])){
            //If the step pin is currently high...
            
            setPinValue(stepPin[DC],LOW); //set step pin low to complete step
            
            //Then increment our encoder value by the required amount of encoder values per step (1 for low speed, 8 for high speed)
            //and in the correct direction (+ = forward, - = reverse).
            unsigned long jVal = cmd.jVal[DC]; 
            jVal = jVal + cmd.stepDir[DC];
            cmd.jVal[DC] = jVal;
            
            if(gotoRunning(DC) && !gotoDecelerating(DC)){
                //If we are currently performing a Go-To and haven't yet started decelleration...
                if (gotoPosn[DC] == jVal){ 
                    //If we have reached the start decelleration marker...
                    setGotoDecelerating(DC); //Mark that we have started decelleration.
                    cmd.currentIVal[DC] = cmd.stopSpeed[DC]+1; //Set the new target speed to slower than the stop speed to cause decelleration to a stop.
                    accelTableRepeatsLeft[DC] = 0;
                }
            } 
            
            if (currentSpeed > cmd.stopSpeed[DC]) {
                //If the current speed is now slower than the stopping speed, we can stop moving. So...
                if(gotoRunning(DC)){ 
                    //if we are currently running a goto... 
                    cmd_setGotoEn(DC,CMD_DISABLED); //Switch back to slew mode 
                    clearGotoRunning(DC); //And mark goto status as complete
                } //otherwise don't as it cancels a 'goto ready' state 
                
                cmd_setStopped(DC,CMD_STOPPED); //mark as stopped 
                timerDisable(DC);  //And stop the interrupt timer.
            } 
        } else {
            //If the step pin is currently low...
            setPinValue(stepPin[DC],HIGH); //Set it high to start next step.
            
            //If the current speed is not the target speed, then we are in the accel/decel phase. So...
            byte repeatsReqd = accelTableRepeatsLeft[DC]; //load the number of repeats left for this accel table entry
            if (repeatsReqd == 0) { 
                //If we have done enough repeats for this entry
                unsigned int targetSpeed = cmd.currentIVal[DC]; //Get the target speed
                if (currentSpeed > targetSpeed) {
                    //If we are going too slow
                    byte accelIndex = accelTableIndex[DC]; //Load the acceleration table index
                    if (accelIndex >= AccelTableLength-1) {
                        //If we are at the top of the accel table
                        currentSpeed = targetSpeed; //Then the new speed is exactly the target speed.
                        accelIndex = AccelTableLength-1; //Ensure index remains in bounds.
                    } else {
                        //Otherwise, we need to accelerate.
                        accelIndex = accelIndex + 1; //Move to the next index
                        accelTableIndex[DC] = accelIndex; //Save the new index back
                        currentSpeed = cmd.accelTable[DC][accelIndex].speed;  //load the new speed from the table
                        if (currentSpeed <= targetSpeed) {
                            //If the new value is too fast
                            currentSpeed = targetSpeed; //Then the new speed is exactly the target speed.
                        } else {
                            //Load the new number of repeats required
                            if (cmd.highSpeedMode[DC]) {
                                //When in high-speed mode, we need to multiply by sqrt(8) ~= 3 to compensate for the change in steps per rev of the motor
                                accelTableRepeatsLeft[DC] = cmd.accelTable[DC][accelIndex].repeats * 3 + 2;
                            } else {
                                accelTableRepeatsLeft[DC] = cmd.accelTable[DC][accelIndex].repeats;
                            }
                        }
                    }
                } else if (currentSpeed < targetSpeed) {
                    //If we are going too fast
                    byte accelIndex = accelTableIndex[DC]; //Load the acceleration table index
                    if (accelIndex == 0) {
                        //If we are at the bottom of the accel table
                        currentSpeed = targetSpeed; //Then the new speed is exactly the target speed.
                    } else {
                        //Otherwise, we need to decelerate.
                        accelIndex = accelIndex - 1; //Move to the next index
                        accelTableIndex[DC] = accelIndex; //Save the new index back
                        currentSpeed = cmd.accelTable[DC][accelIndex].speed;  //load the new speed from the table
                        if (currentSpeed >= targetSpeed) {
                            //If the new value is too slow
                            currentSpeed = targetSpeed; //Then the new speed is exactly the target speed.
                        } else {
                            //Load the new number of repeats required
                            if (cmd.highSpeedMode[DC]) {
                                //When in high-speed mode, we need to multiply by sqrt(8) ~= 3 to compensate for the change in steps per rev of the motor
                                accelTableRepeatsLeft[DC] = cmd.accelTable[DC][accelIndex].repeats * 3 + 2;
                            } else {
                                accelTableRepeatsLeft[DC] = cmd.accelTable[DC][accelIndex].repeats;
                            }
                        }
                    }
                }
                currentMotorSpeed(DC) = currentSpeed; //Update the current speed in case it has changed.
            } else {
                //Otherwise one more repeat done.
                accelTableRepeatsLeft[DC] = repeatsReqd - 1;
            }
        }
    } else {
        //The required number of interrupts have not yet occurred...
        irqToNextStep(DC) = irqToNext; //Update the number of IRQs remaining until the next step.
    }   


}






/*Timer Interrupt Vector*/
ISR(TIMER1_CAPT_vect) {
    
    //Load the number of interrupts until the next step
    unsigned int irqToNext = irqToNextStep(RA)-1;
    //Check if we are ready to step
    if (irqToNext == 0) {
        //Once the required number of interrupts have occurred...
        
        //First update the interrupt base rate using our distribution array. 
        //This affords a more accurate sidereal rate by dithering the intterrupt rate to get higher resolution.
        byte timeSegment = distributionSegment(RA); //Get the current time segement
        
        /* 
        byte index = ((DecimalDistnWidth-1) & timeSegment) >> 1; //Convert time segment to array index
        interruptOVFCount(RA) = timerOVF[RA][index]; //Update interrupt base rate.
        */// Below is optimised version of above:
        byte index = ((DecimalDistnWidth-1) << 1) & timeSegment; //Convert time segment to array index
        interruptOVFCount(RA) = *(int*)((byte*)timerOVF[RA] + index); //Update interrupt base rate.
        
        distributionSegment(RA) = timeSegment + 1; //Increment time segement for next time.

        unsigned int currentSpeed = currentMotorSpeed(RA); //Get the current motor speed
        irqToNextStep(RA) = currentSpeed; //Update interrupts to next step to be the current speed in case it changed (accel/decel)
        
        if (getPinValue(stepPin[RA])){
            //If the step pin is currently high...
            
            setPinValue(stepPin[RA],LOW); //set step pin low to complete step
            
            //Then increment our encoder value by the required amount of encoder values per step (1 for low speed, 8 for high speed)
            //and in the correct direction (+ = forward, - = reverse).
            unsigned long jVal = cmd.jVal[RA]; 
            jVal = jVal + cmd.stepDir[RA];
            cmd.jVal[RA] = jVal;
            
            if(gotoRunning(RA) && !gotoDecelerating(RA)){
                //If we are currently performing a Go-To and haven't yet started decelleration...
                if (gotoPosn[RA] == jVal){ 
                    //If we have reached the start decelleration marker...
                    setGotoDecelerating(RA); //Mark that we have started decelleration.
                    cmd.currentIVal[RA] = cmd.stopSpeed[RA]+1; //Set the new target speed to slower than the stop speed to cause decelleration to a stop.
                    accelTableRepeatsLeft[RA] = 0;
                }
            } 
            
            if (currentSpeed > cmd.stopSpeed[RA]) {
                //If the current speed is now slower than the stopping speed, we can stop moving. So...
                if(gotoRunning(RA)){ 
                    //if we are currently running a goto... 
                    cmd_setGotoEn(RA,CMD_DISABLED); //Switch back to slew mode 
                    clearGotoRunning(RA); //And mark goto status as complete
                } //otherwise don't as it cancels a 'goto ready' state 
                
                cmd_setStopped(RA,CMD_STOPPED); //mark as stopped 
                timerDisable(RA);  //And stop the interrupt timer.
            } 
        } else {
            //If the step pin is currently low...
            setPinValue(stepPin[RA],HIGH); //Set it high to start next step.
            
            //If the current speed is not the target speed, then we are in the accel/decel phase. So...
            byte repeatsReqd = accelTableRepeatsLeft[RA]; //load the number of repeats left for this accel table entry
            if (repeatsReqd == 0) { 
                //If we have done enough repeats for this entry
                unsigned int targetSpeed = cmd.currentIVal[RA]; //Get the target speed
                if (currentSpeed > targetSpeed) {
                    //If we are going too slow
                    byte accelIndex = accelTableIndex[RA]; //Load the acceleration table index
                    if (accelIndex >= AccelTableLength-1) {
                        //If we are at the top of the accel table
                        currentSpeed = targetSpeed; //Then the new speed is exactly the target speed.
                        accelIndex = AccelTableLength-1; //Ensure index remains in bounds.
                    } else {
                        //Otherwise, we need to accelerate.
                        accelIndex = accelIndex + 1; //Move to the next index
                        accelTableIndex[RA] = accelIndex; //Save the new index back
                        currentSpeed = cmd.accelTable[RA][accelIndex].speed;  //load the new speed from the table
                        if (currentSpeed <= targetSpeed) {
                            //If the new value is too fast
                            currentSpeed = targetSpeed; //Then the new speed is exactly the target speed.
                        } else {
                            //Load the new number of repeats required
                            if (cmd.highSpeedMode[RA]) {
                                //When in high-speed mode, we need to multiply by sqrt(8) ~= 3 to compensate for the change in steps per rev of the motor
                                accelTableRepeatsLeft[RA] = cmd.accelTable[RA][accelIndex].repeats * 3 + 2;
                            } else {
                                accelTableRepeatsLeft[RA] = cmd.accelTable[RA][accelIndex].repeats;
                            }
                        }
                    }
                } else if (currentSpeed < targetSpeed) {
                    //If we are going too fast
                    byte accelIndex = accelTableIndex[RA]; //Load the acceleration table index
                    if (accelIndex == 0) {
                        //If we are at the bottom of the accel table
                        currentSpeed = targetSpeed; //Then the new speed is exactly the target speed.
                    } else {
                        //Otherwise, we need to decelerate.
                        accelIndex = accelIndex - 1; //Move to the next index
                        accelTableIndex[RA] = accelIndex; //Save the new index back
                        currentSpeed = cmd.accelTable[RA][accelIndex].speed;  //load the new speed from the table
                        if (currentSpeed >= targetSpeed) {
                            //If the new value is too slow
                            currentSpeed = targetSpeed; //Then the new speed is exactly the target speed.
                        } else {
                            //Load the new number of repeats required
                            if (cmd.highSpeedMode[RA]) {
                                //When in high-speed mode, we need to multiply by sqrt(8) ~= 3 to compensate for the change in steps per rev of the motor
                                accelTableRepeatsLeft[RA] = cmd.accelTable[RA][accelIndex].repeats * 3 + 2;
                            } else {
                                accelTableRepeatsLeft[RA] = cmd.accelTable[RA][accelIndex].repeats;
                            }
                        }
                    }
                }
                currentMotorSpeed(RA) = currentSpeed; //Update the current speed in case it has changed.
            } else {
                //Otherwise one more repeat done.
                accelTableRepeatsLeft[RA] = repeatsReqd - 1;
            }
        }
    } else {
        //The required number of interrupts have not yet occurred...
        irqToNextStep(RA) = irqToNext; //Update the number of IRQs remaining until the next step.
    }   


}

#else
#error Unsupported Part! Please use an Arduino Mega, or ATMega162
#endif

