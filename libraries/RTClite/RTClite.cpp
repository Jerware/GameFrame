/*
This is an extremely pared-down version of the RTClib Arduino library with
no dependency on the Wire library.  Contains only the essential functions
for setting & reading time from the DS1307 RTC, and implements the
low-level TWI/I2C code internally.  Reason being that double-buffered
animation with the RGBmatrixPanel library leaves so little RAM for other
code that the mere act of #including Wire.h would cause it to fail.
So this lets us have a clock and double-buffered animation together.
Some comments were cut for brevity; please refer to the original libraries
for any discussion of their principles of operation.

The RTC code is adapted from public domain code by JeeLabs
http://news.jeelabs.org/code/ -- several functions were stripped out
(only the stuff needed for implementing nice Spectro clocks was kept),
and Wire library calls were replaced with local code.

The TWI code is adapted from the Arduino Wire library, specifically the
lower-level twi functions in libraries/Wire/utility/twi.c.  All slave-
mode functionality is stripped out, and the separate send/receive buffers
are replaced with a single buffer of the minimum size needed to support
RTC communication.
*/

#include <avr/pgmspace.h>
#include "RTClite.h"
#include <avr/io.h>
#include <avr/interrupt.h>
#include <compat/twi.h>

#if (ARDUINO >= 100)
 #include <Arduino.h>
#else
 #include <WProgram.h>
#endif

// -------- TWI/I2C CODE ---------------------------------------------------

#define TWI_READY 0
#define TWI_MRX   1
#define TWI_MTX   2
#define TWI_STX   4
#ifndef TWI_FREQ
#define TWI_FREQ  100000L
#endif
#ifndef cbi
#define cbi(sfr, bit) (_SFR_BYTE(sfr) &= ~_BV(bit))
#endif
#ifndef sbi
#define sbi(sfr, bit) (_SFR_BYTE(sfr) |= _BV(bit))
#endif

#define TWI_BUFFER_LENGTH 9
static volatile uint8_t
  twi_buf[TWI_BUFFER_LENGTH],
  twi_bufIdx,
  twi_state,
  twi_error;
static uint8_t
  twi_bufLen,
  twi_slarw;

static void twi_init(void) {
  twi_state = TWI_READY; // initialize state

  digitalWrite(SDA, HIGH);  // activate internal pullups for twi.
  digitalWrite(SCL, HIGH);

  cbi(TWSR, TWPS0);         // initialize twi prescaler and bit rate
  cbi(TWSR, TWPS1);
  TWBR = ((F_CPU / TWI_FREQ) - 16) / 2;

  TWCR = _BV(TWEN) | _BV(TWIE) | _BV(TWEA); // enable twi, acks, interrupt
}

static void twi_reply(uint8_t ack) {
  // transmit master read ready signal, with or without ack
  TWCR = ack ?
    _BV(TWEN) | _BV(TWIE) | _BV(TWINT) | _BV(TWEA) :
    _BV(TWEN) | _BV(TWIE) | _BV(TWINT);
}

static void twi_stop(void) {
  // send stop condition
  TWCR = _BV(TWEN) | _BV(TWIE) | _BV(TWEA) | _BV(TWINT) | _BV(TWSTO);

  // wait for stop condition to be exectued on bus
  // TWINT is not set after a stop condition!
  while(TWCR & _BV(TWSTO));

  twi_state = TWI_READY; // update twi state
}

static void twi_releaseBus(void) {
  // release bus
  TWCR = _BV(TWEN) | _BV(TWIE) | _BV(TWEA) | _BV(TWINT);

  twi_state = TWI_READY; // update twi state
}

static uint8_t twi_writeTo(uint8_t address, uint8_t length) {
  // ensure data will fit into buffer
  if(TWI_BUFFER_LENGTH < length) return 1;

  // wait until twi is ready, become master transmitter
  while(TWI_READY != twi_state);
  twi_state = TWI_MTX;
  twi_error = 0xFF; // reset error state (0xFF = no error occured)

  // initialize buffer iteration vars
  twi_bufIdx = 0;
  twi_bufLen = length;
  
  // build sla+w, slave device address + w bit
  twi_slarw  = TW_WRITE;
  twi_slarw |= address << 1;
  
  // send start condition
  TWCR = _BV(TWEN) | _BV(TWIE) | _BV(TWEA) | _BV(TWINT) | _BV(TWSTA);

  // wait for write operation to complete
  while(TWI_MTX == twi_state);
  
  if (twi_error == 0xFF)
    return 0;	// success
  else if (twi_error == TW_MT_SLA_NACK)
    return 2;	// error: address send, nack received
  else if (twi_error == TW_MT_DATA_NACK)
    return 3;	// error: data send, nack received
  else
    return 4;	// other twi error
}

uint8_t twi_readFrom(uint8_t address, uint8_t length) {
  // ensure data will fit into buffer
  if(TWI_BUFFER_LENGTH < length) return 0;

  // wait until twi is ready, become master receiver
  while(TWI_READY != twi_state);
  twi_state = TWI_MRX;
  twi_error = 0xFF; // reset error state (0xFF = no error occured)

  // initialize buffer iteration vars
  twi_bufIdx = 0;
  twi_bufLen = length-1;  // This is not intuitive, read on...
  // On receive, the previously configured ACK/NACK setting is transmitted in
  // response to the received byte before the interrupt is signalled. 
  // Therefor we must actually set NACK when the _next_ to last byte is
  // received, causing that NACK to be sent in response to receiving the last
  // expected byte of data.

  // build sla+w, slave device address + w bit
  twi_slarw  = TW_READ;
  twi_slarw |= address << 1;

  // send start condition
  TWCR = _BV(TWEN) | _BV(TWIE) | _BV(TWEA) | _BV(TWINT) | _BV(TWSTA);

  // wait for read operation to complete
  while(TWI_MRX == twi_state);

  if(twi_bufIdx < length) length = twi_bufIdx;

  return length;
}

ISR(TWI_vect) {
  switch(TW_STATUS) {
   // All Master
   case TW_START:     // sent start condition
   case TW_REP_START: // sent repeated start condition
    // copy device address and r/w bit to output register and ack
    TWDR = twi_slarw;
    twi_reply(1);
    break;

   // Master Transmitter
   case TW_MT_SLA_ACK:  // slave receiver acked address
   case TW_MT_DATA_ACK: // slave receiver acked data
    // if there is data to send, send it, otherwise stop 
    if(twi_bufIdx < twi_bufLen) {
      // copy data to output register and ack
      TWDR = twi_buf[twi_bufIdx++];
      twi_reply(1);
    } else {
      twi_stop();
    }
    break;
   case TW_MT_SLA_NACK:  // address sent, nack received
    twi_error = TW_MT_SLA_NACK;
    twi_stop();
    break;
   case TW_MT_DATA_NACK: // data sent, nack received
    twi_error = TW_MT_DATA_NACK;
    twi_stop();
    break;
   case TW_MT_ARB_LOST: // lost bus arbitration
    twi_error = TW_MT_ARB_LOST;
    twi_releaseBus();
    break;

   // Master Receiver
   case TW_MR_DATA_ACK: // data received, ack sent
    // put byte into buffer
    twi_buf[twi_bufIdx++] = TWDR;
   case TW_MR_SLA_ACK:  // address sent, ack received
    // ack if more bytes are expected, otherwise nack
    twi_reply((twi_bufIdx < twi_bufLen) ? 1 : 0);
    break;
   case TW_MR_DATA_NACK: // data received, nack sent
    // put final byte into buffer
    twi_buf[twi_bufIdx++] = TWDR;
    case TW_MR_SLA_NACK: // address sent, nack received
    twi_stop();
    break;
   // TW_MR_ARB_LOST handled by TW_MT_ARB_LOST case

   // All
   case TW_NO_INFO:   // no state information
    break;
   case TW_BUS_ERROR: // bus error, illegal stop/start
    twi_error = TW_BUS_ERROR;
    twi_stop();
    break;
  }
}

// -------- RTC CODE -------------------------------------------------------

#define DS1307_ADDRESS            0x68
#define SECONDS_FROM_1970_TO_2000 946684800

const uint8_t daysInMonth [] PROGMEM = { 31,28,31,30,31,30,31,31,30,31,30,31 };

DateTime::DateTime(uint32_t t) {
  t -= SECONDS_FROM_1970_TO_2000; // bring to 2000 timestamp from 1970
  ss = t % 60;
  t /= 60;
  mm = t % 60;
  t /= 60;
  hh = t % 24;

  uint16_t days = t / 24;
  uint8_t  leap;
  for(yOff=0; ; ++yOff) {
    leap = (yOff % 4 == 0);
    if(days < (365 + leap)) break;
    days -= 365 + leap;
  }
  for(m=1; ; ++m) {
    uint8_t daysPerMonth = pgm_read_byte(daysInMonth + m - 1);
    if(leap && (m == 2)) ++daysPerMonth;
    if(days < daysPerMonth) break;
    days -= daysPerMonth;
  }
  d = days + 1;
}

DateTime::DateTime(uint16_t year, uint8_t month, uint8_t day,
  uint8_t hour, uint8_t min, uint8_t sec) {
  if(year >= 2000) year -= 2000;
  yOff = year;
  m    = month;
  d    = day;
  hh   = hour;
  mm   = min;
  ss   = sec;
}

static uint8_t conv2d(const char* p) {
  uint8_t v = 0;
  if(('0' <= *p) && (*p <= '9')) v = *p - '0';
  return 10 * v + *++p - '0';
}

// A convenient constructor for using "the compiler's time":
//   DateTime now (__DATE__, __TIME__);
DateTime::DateTime(const char* date, const char* time) {
  // sample input: date = "Dec 26 2009", time = "12:34:56"
  yOff = conv2d(date + 9);
  // Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec 
  switch (date[0]) {
    case 'J': m = (date[1]=='a') ? 1 : (m = (date[2]=='n') ? 6 : 7); break;
    case 'F': m = 2; break;
    case 'A': m = (date[2]=='r') ? 4 : 8; break;
    case 'M': m = (date[2]=='r') ? 3 : 5; break;
    case 'S': m = 9; break;
    case 'O': m = 10; break;
    case 'N': m = 11; break;
    case 'D': m = 12; break;
  }
  d  = conv2d(date + 4);
  hh = conv2d(time);
  mm = conv2d(time + 3);
  ss = conv2d(time + 6);
}

// number of days since 2000/01/01, valid for 2001..2099
static uint16_t date2days(uint16_t y, uint8_t m, uint8_t d) {
  if(y >= 2000) y -= 2000;
  uint16_t days = d;
  for (uint8_t i = 1; i < m; ++i)
    days += pgm_read_byte(daysInMonth + i - 1);
  if((m > 2) && ((y % 4) == 0)) ++days;
  return days + 365 * y + (y + 3) / 4 - 1;
}

uint8_t DateTime::dayOfWeek() const {    
  return (date2days(yOff, m, d) + 6) % 7; // Jan 1, 2000 is a Sat, returns 6
}

static uint8_t bcd2bin(uint8_t val) { return val - 6 * (val >> 4); }
static uint8_t bin2bcd(uint8_t val) { return val + 6 * (val / 10); }

int8_t RTC_DS1307::begin(void) {
  twi_init();
  return 1;
}

uint8_t RTC_DS1307::isrunning(void) {
  twi_buf[0] = 0;
  twi_writeTo(DS1307_ADDRESS, 1);

  // perform blocking read into buffer
  (void)twi_readFrom(DS1307_ADDRESS, 1);
  uint8_t ss = twi_buf[0];
  return !(ss>>7);
}

void RTC_DS1307::adjust(const DateTime& dt) {
  twi_buf[0] = 0;
  twi_buf[1] = bin2bcd(dt.second());
  twi_buf[2] = bin2bcd(dt.minute());
  twi_buf[3] = bin2bcd(dt.hour());
  twi_buf[4] = bin2bcd(0);
  twi_buf[5] = bin2bcd(dt.day());
  twi_buf[6] = bin2bcd(dt.month());
  twi_buf[7] = bin2bcd(dt.year() - 2000);
  twi_buf[8] = 0;
  twi_writeTo(DS1307_ADDRESS, 9);
}

DateTime RTC_DS1307::now() {
  twi_buf[0] = 0;
  twi_writeTo(DS1307_ADDRESS, 1);

  (void)twi_readFrom(DS1307_ADDRESS, 7);

  uint8_t ss = bcd2bin(twi_buf[0] & 0x7F);
  uint8_t mm = bcd2bin(twi_buf[1]);
  uint8_t hh = bcd2bin(twi_buf[2]);
  uint8_t d  = bcd2bin(twi_buf[4]); // #3 IS SKIPPED
  uint8_t m  = bcd2bin(twi_buf[5]);
  uint16_t y = bcd2bin(twi_buf[6]) + 2000;

  return DateTime(y, m, d, hh, mm, ss);
}

