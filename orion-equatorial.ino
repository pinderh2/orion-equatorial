/*
 * This is the software for a telescope motor drive.
 * 
 * This is a platform drive intended for Orion XT10. It has a south bearing with axis that points towards
 * Polaris, and two north roller bearings that support a circular arc built into the platform. This allows 
 * smooth rotation on an axis parallel to the earth's rotation. 
 * 
 * This design has an inherent physical limitation of handling about 50 minutes of rotation end-to-end.
 * 
 * This software is intended for Arduino Uno, although it should work fine on a range of models.
 * 
 * The hardware design consists of 
 *  - the Arduino
 *  - a NEMA17 stepper motor connected to the platform via a belt drive system
 *  - a stepper motor driver, type a4988.  
 *      (see https://www.makerguides.com/a4988-stepper-motor-driver-arduino-tutorial/)
 *  - a 4-digit 7-segment display module, of TM1637 type 
 *  - a single momentary push button for control
 *  
 *  The operational design is pretty simple:
 *  - an Idle state, with stepper motor disabled so you can position the platform freely,
 *       usually to set it to the starting end of its range.
 *  - a Hold state, with the stepper motor enabled but not running yet, to allow the telescope to 
 *       be positioned at the target while the base is stable.
 *  - a Run state, when the base is driven.
 *  
 *  To drive the TM1637 disply I used the library found here: https://github.com/avishorp/TM1637
 *    (I found the Grove 4-digit display driver by Seeed Studio did not work correctly with my display.)
 *    
 */

#include <TM1637Display.h>
#include <TimerOne.h>

// These are the pins going to the stepper motor driver
#define STEPPER_DIR_PIN 2
#define STEPPER_STEP_PIN 3
#define STEPPER_ENABLE_PIN 4

// These are the pins going to the 4-digit 7 segment display
#define TM1637_DATA_PIN 8
#define TM1637_CLK_PIN 9

// This pin goes to the single momentary push button, for control
#define MODE_BUTTON_PIN 10

/*  Here is the math behind the driving speed.
 *   
 *  The driving shaft is 8mm in diameter, or 4mm radius.
 *  The big thing being driven is 539.5 mm radius. 
 *  This gives a gear ratio of 4/539.5
 *  
 *  The drive gear has 20 teeth and the driven gear has 40.
 *  
 *  We want the thing being driven to have one rotation in 24 hours, just like the Earth,
 *  or 1/24 rotation in 1 hour.
 *  
 *  That means the motor turns (539.5/4) (1/24) (40/20) rotation in one hour.
 *  This is 11.2396 rotations in one hour.
 *  
 *  Here is another way to calculate that, using dimensional analysis:
 *  
 *  1 big rot   539.5 pl rot   40 mot rot   1 hr     200 step   16 uStp          uStp
 *  --------- * ------------ * ---------- * ------ * -------- * ------- = 9.9907 ----
 *  24 hour      4 big rot     20 pl rot    3600 s   1 mot rot   1 step            s
 *  
 *  (pl means pulley, which is the 40-tooth belt pulley on the driving shaft.)
 *  
 *  or 100,093 microseconds per uStp.
 *  
 */
#define USEC_PER_USTEP 100093

/* Our physical design was meant to go for one hour.
 * However...  somehow in reality it only goes for 50 minutes before hitting a stop.
 * Didn't get all the mechanicals just right... oh well...
 * 
 * This represents 50 * 60 * 1000 milliseconds
 *   or 3000000 milliseconds
 */
#define MAX_DURATION_MSEC 3000000
 
/* This variable indicates whether stepping should occur or not.
 *  It is basically a signal from mainline code to the ISR.
 *  Since it is a single byte, it can be safely written by mainline code without disabling interrupts.
 */
bool steppingOn = false;

/* Instantiate a 4-segment LED module */
TM1637Display tm1637(TM1637_CLK_PIN, TM1637_DATA_PIN);

/* There is a way to simulate an accelerated time mode, for debug.
   Enable this mode by booting with the button pressed.
   Usually this is a 60x mode, so minutes become seconds.
   But the default is the normal time mode, with multiplier = 1.
 */
int debugTimeMult = 1;

void setup() {

  pinMode(MODE_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(MODE_BUTTON_PIN) == 0) {
    debugTimeMult = 60;   /* go to 60x mode if booting with button pressed */
  }

  pinMode(STEPPER_STEP_PIN, OUTPUT);
  pinMode(STEPPER_DIR_PIN, OUTPUT);
  pinMode(STEPPER_ENABLE_PIN, OUTPUT);
  digitalWrite(STEPPER_STEP_PIN, LOW);
  digitalWrite(STEPPER_DIR_PIN, LOW);  /* this is the correct direction based on mechanical design.
                                          the software never attempts to drive the motor backwards. */
  digitalWrite(STEPPER_ENABLE_PIN, HIGH);

  tm1637.setBrightness(0x00);
  
  Timer1.initialize(USEC_PER_USTEP / debugTimeMult);
  Timer1.attachInterrupt(takeStepISR);
}

/*
 * This is the ISR, triggered by the timer interrupt.
 * Main purpose is to take one step, assuming that one is needed.
 */
void takeStepISR(void)
{
  if (steppingOn) {
    digitalWrite(STEPPER_STEP_PIN, HIGH);
    delayMicroseconds(100);
    digitalWrite(STEPPER_STEP_PIN, LOW);
  }
}


/*
 * This fetches the current time, in milliseconds.
 * Normally, this just returns 'millis()'.
 * But we have a way to run time "faster", for debug.  In that case this returns
 * a multiplier of millis().
 */
unsigned long myMillis()
{
  return (millis() * debugTimeMult);
}


/*  Display a time value on the 4-digit display.
 *  The time is given in milliseconds, however it is displayed in MM:SS format.
 *  This updates the display only if needed (i.e. only if the displayed time will be different
 *    than the last displayed time).
 *  A msecTime value equal to or greater than 100 minutes will be interpreted as a
 *    negative number (i.e. timer has elapsed) and will be displayed as 00:00.
 */
void dispTime(unsigned long msecTime) 
{
  int8_t TimeDisp[4];
  int second, minute;
  static int8_t LastTimeDisp[4] = {0,0,0,0};
  unsigned long secTime;

  secTime = (msecTime + 999) / 1000;
  /* Adding 999 has the effect of rounding up to the nearest second, to counteract the natural rounding-down of using 
   * integer division.  Rounding up is better because it will display the starting time for a full second, instead 
   * of for just one millisecond */
  
  if (msecTime >= 6000000) {
    second = 0;
    minute = 0;
  } else {
    second = secTime % 60;
    minute = secTime / 60;
  }
  
  TimeDisp[0] = minute / 10;
  TimeDisp[1] = minute % 10;
  TimeDisp[2] = second / 10;
  TimeDisp[3] = second % 10;
  
  if (memcmp(TimeDisp, LastTimeDisp, 4) != 0) {
    TimeDisp[0] = tm1637.encodeDigit(TimeDisp[0]);
    TimeDisp[1] = tm1637.encodeDigit(TimeDisp[1]) | 0x80;   /* this turns on the dots separator */
    TimeDisp[2] = tm1637.encodeDigit(TimeDisp[2]);
    TimeDisp[3] = tm1637.encodeDigit(TimeDisp[3]);
    tm1637.setSegments(TimeDisp);
    memcpy(LastTimeDisp, TimeDisp, 4);
  }
}

/* Define the states used by the simple state machine */
#define ST_INIT 0      /* Default initial state, on power-up  */
#define ST_IDLE 1      /* Idle state - not stepping, and motor off */
#define ST_HOLD 2      /* Hold state - not stepping, but motor is on */
#define ST_RUN  3      /* Run state - motor on and stepping. Display time remaining */

/*
 * Main loop, gets called over and over.
 */
void loop() {
  bool buttonReading;
  bool buttonWasPressed;
  static bool lastButtonReading = 0;
  static unsigned long stopTime;
  static int state = ST_INIT;
  int newState;
  int8_t HoldDisp[4] = {0b01110110,0b01011100,0b00111000,0b01011110};
  int8_t IdleDisp[4] = {0b00000110,0b01011110,0b00111000,0b01111001};
// This is the pattern of the segments, FYI
//      A
//     ---
//  F |   | B
//     -G-
//  E |   | C
//     ---
//      D
// XGFEDCBA

  /* See if someone has pressed the button */
  buttonReading = digitalRead(MODE_BUTTON_PIN);
  buttonWasPressed = (buttonReading == 0) && (lastButtonReading == 1);
  lastButtonReading = buttonReading; 

  /* Determine if a state transition is required. */
  newState = state;
  switch (state) {
    
    case ST_INIT:
      newState = ST_IDLE;    /* always move directly into the Idle state at powerup */
      break;
      
    case ST_IDLE:
      if (buttonWasPressed) 
        newState = ST_HOLD;  /* Button press gets us from Idle to Hold */
      break;
      
    case ST_HOLD:
      if(buttonWasPressed) 
        newState = ST_RUN;   /* Button press gets us from Hold to Run */
      break;
      
    case ST_RUN:
      if(buttonWasPressed || (myMillis() >= stopTime) )  /* WARNING - does not handle time rollover case! 
                                                                     (which happens after 50 days, so ignore ...) */
        newState = ST_IDLE;   /* Button press, or time running out gets us back to Idle */
      break;
      
    default:
      break;
  }

  /* Take actions based on any state transition that happened */
  if (state != newState) {
    switch (newState) {
      
      case ST_IDLE:
        tm1637.setSegments(IdleDisp);
        digitalWrite(STEPPER_ENABLE_PIN, HIGH);   /* Turn off the motor */
        steppingOn = false; 
        break;
      
      case ST_HOLD:
        tm1637.setSegments(HoldDisp);
        steppingOn = false; 
        digitalWrite(STEPPER_ENABLE_PIN, LOW);    /* Turn on the motor */
        break;
      
      case ST_RUN:
        dispTime((unsigned long) MAX_DURATION_MSEC);
        steppingOn = true;                        /* Start the stepping */ 
        digitalWrite(STEPPER_ENABLE_PIN, LOW);    /* Turn on the motor (should be on from state 2, so just in case...) */
        stopTime = myMillis() + (unsigned long) MAX_DURATION_MSEC;    /* start the timer */
        break;
        
      default:
        break;
    }
    state = newState;
  }

  /* Take actions based on being in a certain state */
  if (state == ST_RUN) {
    dispTime ( stopTime - myMillis() );
  }

  /*  Run nominally 100 times per second. This gives smooth operation of the time display. */
  delay(10);

}
