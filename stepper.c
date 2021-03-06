/*
  stepper.c - stepper motor interface
  Part of Grbl

  Copyright (c) 2009 Simen Svale Skogsrud

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

/* The timer calculations of this module informed by the 'RepRap cartesian firmware' by Zack Smith
   and Philipp Tiefenbacher. The ring buffer implementation gleaned from the wiring_serial library 
   by David A. Mellis */

#include "stepper.h"
#include "config.h"
#include <math.h>
#include <stdlib.h>
#include <util/delay.h>
#include "nuts_bolts.h"
#include <avr/interrupt.h>
#include "wiring_serial.h"


#include <avr/io.h>
#include "serial_protocol.h"
#include "gcode.h"
#include "motion_control.h"
#include <avr/pgmspace.h>


//extern volatile char mc_running;
char buttons_in_use=0;
StepModeT st_current_mode=SM_RUN;

// Pick a suitable block-buffer size
#ifdef __AVR_ATmega328P__
#define BLOCK_BUFFER_SIZE 20   // Atmega 328 has one full kilobyte of extra RAM!
#else
#define BLOCK_BUFFER_SIZE 10
#endif

// This record is used to buffer the setup for each motion block
struct Block {
  uint32_t  steps_x, steps_y, steps_z;  // Step count along each axis
  uint32_t  pos_x, pos_y, pos_z;	    // Initial position on line
  int32_t   maximum_steps;              // The largest stepcount of any axis for this block
  uint8_t   direction_bits;             // The direction bit set for this block (refers to *_DIRECTION_BIT in config.h)
  uint32_t  rate;                       // The nominal step rate for this block in microseconds/step
  uint8_t	backlash;					// Set true if the block is for backlash compensation
  int line_number;						// Holds line number of current block
  StepModeT mode;						// Holds whether the next steps are run or
  										// delay. If in delay mode, then the delay
  										// lasts for maximum_steps of rate each
};

extern int32_t position[3];    // The current position of the tool in absolute steps

int32_t actual_position[3];    	// The current position of the tool in absolute steps
								// In this file, this is actually the current position, not 
								// estimated current position...

struct Block block_buffer[BLOCK_BUFFER_SIZE]; // A buffer for step instructions
volatile int block_buffer_head = 0;
volatile int block_buffer_tail = 0;

#define ENABLE_STEPPER_DRIVER_INTERRUPT()  TIMSK1 |= (1<<OCIE1A)
#define DISABLE_STEPPER_DRIVER_INTERRUPT() TIMSK1 &= ~(1<<OCIE1A)


// Variables used by SIG_OUTPUT_COMPARE1A
uint8_t out_bits;               // The next stepping-bits to be output
struct Block *current_block;    // A pointer to the block currently being traced
volatile int32_t counter_x, 
                 counter_y, 
                 counter_z;     // counter variables for the bresenham line tracer
uint32_t iterations;            // The number of iterations left to complete the current_block
volatile int busy;              // TRUE when SIG_OUTPUT_COMPARE1A is being serviced. Used to avoid retriggering that handler.

uint8_t old_direction_bits;		// Holds the direction bits from the previous
								// call to st_buffer_block, to 
								// determine if backlash compensation is required.

extern char buttons[4];

void config_step_timer(uint32_t microseconds);

char st_buffer_full()
{
//In cases where there is a change of direction, two items 
// get put onto the st_buffer (the backlash compensation step
// and the actual movement). So this check for space in the
// buffer checks to see if there are two spaces left.

// There is also a race condition that may arise while drawing
// arcs: when status is checked, the buffer shows one element
// free and reports it, but by the time the command arrives
// to add another block, the arc code has filled the buffer
// and there is an error. So the buffer is reported full
// if arc adding is still going on.

  int nb1;
  int nb2;
  nb2 = (block_buffer_head + 2) % BLOCK_BUFFER_SIZE;	
  nb1 = (block_buffer_head + 1) % BLOCK_BUFFER_SIZE;	

  return ((nb1 == block_buffer_tail)||
          (nb2 == block_buffer_tail));
}

int st_buffer_delay(uint32_t milliseconds, int16_t line_number) {
  struct Block 	*block;

  if (milliseconds==0) {
    st_stop();
  	return 0;
  }
	
  while (st_buffer_full()) { sleep_mode();}  // makes sure there are two

/*                                             // slots left on buffer
 printPgmString(PSTR("in st_buffer_delay. Delay = "));
 printInteger(milliseconds);
 printPgmString(PSTR("\r\n"));*/
 
 
   // Setup block record
  block = &block_buffer[block_buffer_head];
  block->backlash=0;
  block->line_number = line_number;
  
  block->maximum_steps = milliseconds;
  block->rate = 1000;
  block->mode = SM_HALT;

  // Move buffer head
  block_buffer_head = (block_buffer_head + 1) % BLOCK_BUFFER_SIZE;	//next_buffer_head;
  
  // Ensure that blocks will be processed by enabling the Stepper Driver Interrupt
  ENABLE_STEPPER_DRIVER_INTERRUPT();
  return 1;
}

// *************************************************************************************************
// Add a new linear movement to the buffer. steps_x, _y and _z is the signed, relative motion in 
// steps. Microseconds specify how many microseconds the move should take to perform.
//
// DV: 
// 1. Works out how many steps to go in each direction
// 2. Takes total time and divides by number of steps to get time for one step
// 3. Put time per step, and number of steps in x, y, z into block record. 
// 4. Enable timer.

int st_buffer_block(int32_t steps_x, int32_t steps_y, int32_t steps_z,
					int32_t pos_x,   int32_t pos_y,   int32_t pos_z,
					uint32_t microseconds, 
					int16_t line_number) {
  uint8_t 		direction_bits = 0;
  uint8_t 		changed_dir;
  struct Block 	*block;
  struct Block 	*comp_block=NULL;
  uint32_t		maximum_steps;
  
  maximum_steps = max(labs(steps_x), max(labs(steps_y), labs(steps_z)));
  // Don't process empty blocks 
  if (maximum_steps==0) {return 0;}
  
  // Determine direction bits					
  if (steps_x < 0) { direction_bits |= (1<<X_DIRECTION_BIT); }
  if (steps_y < 0) { direction_bits |= (1<<Y_DIRECTION_BIT); }
  if (steps_z < 0) { direction_bits |= (1<<Z_DIRECTION_BIT); }
	
  while (st_buffer_full()) { sleep_mode();}  // makes sure there are two
                                             // slots left on buffer
                                            
  // If direction has changed, then put a backlash instruction				
  // on the queue:
  if (direction_bits!=old_direction_bits){
  	comp_block = &block_buffer[block_buffer_head];

	comp_block->backlash = 1;
  	comp_block->direction_bits = direction_bits;
  	comp_block->line_number = line_number;
	
    comp_block->steps_x = 0;
    comp_block->steps_y = 0;
    comp_block->steps_z = 0;  
    
    comp_block->pos_x = pos_x;
    comp_block->pos_y = pos_y;
    comp_block->pos_z = pos_z;  

	changed_dir = direction_bits^old_direction_bits;

  	old_direction_bits = direction_bits;

	if (changed_dir & (1<<X_DIRECTION_BIT)) comp_block->steps_x=settings.backlash_x_count;
	if (changed_dir & (1<<Y_DIRECTION_BIT)) comp_block->steps_y=settings.backlash_y_count;
	if (changed_dir & (1<<Z_DIRECTION_BIT)) comp_block->steps_z=settings.backlash_z_count;
	  
	// Use same rate for backlash compensation as for the block itself:
  	comp_block->rate = microseconds/maximum_steps;
  	comp_block->mode = SM_RUN;

  	comp_block->maximum_steps = max(comp_block->steps_x, max(comp_block->steps_y, comp_block->steps_z));
  // If compensation is not empty, advance the end of the queue 
  	if (comp_block->maximum_steps > 0) {
	  	block_buffer_head = (block_buffer_head + 1) % BLOCK_BUFFER_SIZE;
	}
  }

  // Setup block record
  block = &block_buffer[block_buffer_head];
  block->backlash=0;
  block->line_number = line_number;
  
  block->steps_x = labs(steps_x);
  block->steps_y = labs(steps_y);
  block->steps_z = labs(steps_z);  
    
  block->pos_x = pos_x;
  block->pos_y = pos_y;
  block->pos_z = pos_z;  
  
  block->maximum_steps = maximum_steps;

  block->rate = microseconds/block->maximum_steps;
  block->mode = SM_RUN;
  // Compute direction bits for this block
  block->direction_bits = direction_bits;
  
  // Move buffer head
  block_buffer_head = (block_buffer_head + 1) % BLOCK_BUFFER_SIZE;	//next_buffer_head;
  
  // Ensure that blocks will be processed by enabling The Stepper Driver Interrupt
  ENABLE_STEPPER_DRIVER_INTERRUPT();
    return 1;
}
// *************************************************************************************************

// "The Stepper Driver Interrupt" - This timer interrupt is the workhorse of Trbl. It is  executed at the rate set with
// config_step_timer. It pops blocks from the block_buffer and executes them by pulsing the stepper pins appropriately. 
// It is supported by The Stepper Port Reset Interrupt which it uses to reset the stepper port after each pulse.
// 
// DV:
// If this is a new block:
// 	 1. Set up counters
// 	 2. Set up timer
// 	 3. Fire interupt
// else if we're already ticking:
//   1. On each of x, y, z: 
//		add number of steps in direction
//		

char st_process_manual_buttons(void);

#ifdef TIMER1_COMPA_vect
SIGNAL(TIMER1_COMPA_vect)
#else
SIGNAL(SIG_OUTPUT_COMPARE1A)
#endif
{
  if(busy){ return; } // The busy-flag is used to avoid reentering this interrupt
  STEPPERS_ENABLE_PORT |= (1<<STEPPERS_ENABLE_BIT);
  // Set the direction pins a cuple of nanoseconds before we step the steppers
  STEPPING_PORT = (STEPPING_PORT & ~DIRECTION_MASK) | (out_bits & DIRECTION_MASK);
  // Then pulse the stepping pins
  STEPPING_PORT = (STEPPING_PORT & ~STEP_MASK) | out_bits;
  // Reset step pulse reset timer so that SIG_OVERFLOW2 can reset the signal after
  // exactly settings.pulse_microseconds microseconds.
  TCNT2 = -(((settings.pulse_microseconds-2)*TICKS_PER_MICROSECOND)/8);

  busy = TRUE;
  sei(); // Re enable interrupts (normally disabled while inside an interrupt handler)
 		 // We re-enable interrupts in order for SIG_OVERFLOW2 to be able to be triggered 
 		 // at exactly the right time even if we occasionally spend a lot of time inside this handler.
        
  // If there is no current block, attempt to pop one from the buffer
  if (current_block == NULL) {
    // Anything in the buffer?
    if (block_buffer_head != block_buffer_tail) {
      // Retrieve a new block and get ready to step it
      current_block = &block_buffer[block_buffer_tail];
      acting_line_number = current_block->line_number;
      st_current_mode = current_block->mode;
      config_step_timer(current_block->rate);
      iterations = current_block->maximum_steps;
      if (st_current_mode == SM_RUN){
		  counter_x = -(current_block->maximum_steps >> 1);
		  counter_y = counter_x;
		  counter_z = counter_x;
		  actual_position[X_AXIS] = current_block->pos_x;
		  actual_position[Y_AXIS] = current_block->pos_y;
		  actual_position[Z_AXIS] = current_block->pos_z;
	  } else {
	  // In SM_HALT mode

		  if (iterations==0){
		  // Gcode put a stop on the buffer that has now arrived.
			st_flush();
			current_block=NULL;
			out_bits=0;
			DISABLE_STEPPER_DRIVER_INTERRUPT();
			STEPPERS_ENABLE_PORT &= ~(1<<STEPPERS_ENABLE_BIT);
			mc_running = 0;
		  }
	  }      
    } else {
	  if (buttons[0]|buttons[1]|buttons[2]|buttons[3]) {
	    buttons_in_use=1;
	  	st_process_manual_buttons();
	  	acting_line_number = 0;
		STEPPERS_ENABLE_PORT |= (1<<STEPPERS_ENABLE_BIT);
		ENABLE_STEPPER_DRIVER_INTERRUPT();
        mc_running=1;
      } else {
	    if (buttons_in_use){
	      buttons_in_use=0;
	      memcpy(position, actual_position, sizeof(position)); // position[] = actual_position[]
	      set_gcPosition(position);
        }
      	// Buffer empty. Disable this interrupt until there is something to handle
		out_bits=0;
      	DISABLE_STEPPER_DRIVER_INTERRUPT();
	  	STEPPERS_ENABLE_PORT &= ~(1<<STEPPERS_ENABLE_BIT);
	  	mc_running = 0;
	  }
    }    
  } 

  if (current_block != NULL) {
	mc_running=1;
	out_bits = current_block->direction_bits;
  	if (current_block->mode == SM_RUN){
		counter_x += current_block->steps_x;
		if (counter_x > 0) {
		  out_bits |= (1<<X_STEP_BIT);
		  counter_x -= current_block->maximum_steps;
		  if (out_bits & (1<<X_DIRECTION_BIT)){
			actual_position[X_AXIS]-=1;
		  } else {
			actual_position[X_AXIS]+=1;
		  }
		}
		counter_y += current_block->steps_y;
		if (counter_y > 0) {
		  out_bits |= (1<<Y_STEP_BIT);
		  counter_y -= current_block->maximum_steps;
		  if (out_bits & (1<<Y_DIRECTION_BIT)){
			actual_position[Y_AXIS]-=1;
		  } else {
			actual_position[Y_AXIS]+=1;
		  }
		}
		counter_z += current_block->steps_z;
		if (counter_z > 0) {
		  out_bits |= (1<<Z_STEP_BIT);
		  counter_z -= current_block->maximum_steps;
		  if (out_bits & (1<<Z_DIRECTION_BIT)){
			actual_position[Z_AXIS]-=1;
		  } else {
			actual_position[Z_AXIS]+=1;
		  }
		} else {
	        // Support delays - nothing actually to do!
		}
	}	
	// If current block is finished, reset pointer 
	iterations -= 1;
	if (iterations <= 0) {
	  current_block = NULL;
	  // move the block buffer tail to the next instruction
	  block_buffer_tail = (block_buffer_tail + 1) % BLOCK_BUFFER_SIZE;      
	}
  }
  out_bits ^= settings.invert_mask;
  busy=FALSE;
  // Done. The next time this interrupt is entered the out_bits we just calculated will be pulsed onto the port
}


// "The Stepper Port Reset Interrupt" - This interrupt is set up by The Stepper Driver Interrupt when it sets the 
// motor port bits. It resets the motor port after a short period (settings.pulse_microseconds) completing one step cycle.
#ifdef TIMER2_OVF_vect
SIGNAL(TIMER2_OVF_vect)
#else
SIGNAL(SIG_OVERFLOW2)
#endif
{
  // reset stepping pins (leave the direction pins)
  STEPPING_PORT = (STEPPING_PORT & ~STEP_MASK) | (settings.invert_mask & STEP_MASK); 
}

// Initialize and start the stepper motor subsystem
void st_init()
{
	// Configure directions of interface pins
  STEPPING_DDR   |= STEPPING_MASK;
  STEPPING_PORT = (STEPPING_PORT & ~STEPPING_MASK) | settings.invert_mask;
  LIMIT_DDR &= ~(LIMIT_MASK);
  STEPPERS_ENABLE_DDR |= 1<<STEPPERS_ENABLE_BIT;
  
	// waveform generation = 0100 = CTC
	TCCR1B &= ~(1<<WGM13);
	TCCR1B |=  (1<<WGM12);
	TCCR1A &= ~(1<<WGM11); 
	TCCR1A &= ~(1<<WGM10);

	// output mode = 00 (disconnected)
	TCCR1A &= ~(3<<COM1A0); 
	TCCR1A &= ~(3<<COM1B0); 
	
	// Configure Timer 2
  TCCR2A = 0; // Normal operation
  TCCR2B = (1<<CS21); // Full speed, 1/8 prescaler
  TIMSK2 |= (1<<TOIE2);      
  
  // Just set the step_timer to something serviceably lazy
  config_step_timer(20000);
  // set enable pin     
  STEPPERS_ENABLE_PORT |= 1<<STEPPERS_ENABLE_BIT;
  
  old_direction_bits=0;
  
  sei();
}

/*
// Block until all buffered steps are executed
void st_synchronize()
{
  while(block_buffer_tail != block_buffer_head) { sleep_mode(); }    
}
*/
// Cancel all buffered steps
void st_flush()
{
  cli();
  block_buffer_tail = block_buffer_head;
  current_block = NULL;
  sei();
}

// Configures the prescaler and ceiling of timer 1 to produce 
// the given rate as accurately as possible.
void config_step_timer(uint32_t microseconds)
{
  uint32_t ticks = microseconds*TICKS_PER_MICROSECOND;
  uint16_t ceiling;
  uint16_t prescaler;
	if (ticks <= 0xffffL) {
		ceiling = ticks;
    	prescaler = 0; // prescaler: 0
	} else if (ticks <= 0x7ffffL) {
    	ceiling = ticks >> 3;
   	 	prescaler = 1; // prescaler: 8
	} else if (ticks <= 0x3fffffL) {
		ceiling =  ticks >> 6;
    	prescaler = 2; // prescaler: 64
	} else if (ticks <= 0xffffffL) {
		ceiling =  (ticks >> 8);
    	prescaler = 3; // prescaler: 256
	} else if (ticks <= 0x3ffffffL) {
		ceiling = (ticks >> 10);
    	prescaler = 4; // prescaler: 1024
	} else {
	  // Okay, that was slower than we actually go. Just set the slowest speed
		ceiling = 0xffff;
    	prescaler = 4;
	}
	// Set prescaler
  TCCR1B = (TCCR1B & ~(0x07<<CS10)) | ((prescaler+1)<<CS10);
  // Set ceiling
  OCR1A = ceiling;
}


void st_go_home()
{
  // Todo: Perform the homing cycle
}


void st_stop()
{
	st_flush();
	current_block=NULL;		// Rather brutal, but works!
	
	memcpy(position, actual_position, sizeof(position)); // position[] = actual_position[]

}

extern char buttons[4];

// DEFAULT_X_STEPS_PER_MM (200/1.27*MICROSTEPS)
// DEFAULT_SEEKRATE 480.0 // in millimeters per minute
// So 480 mm = 480 * 1200 = 500 000 / min = 10 000 steps/second = 1e6/10 000 = 100us
// ONE_MINUTE_OF_MICROSECONDS 60000000.0


// DEFAULT_SEEKRATE * DEFAULT_X_STEPS_PER_MM = STEPS/MINUTE
// ONE_MINUTE_OF _MICROSECONDS / STEPS/MINUTE = TIME PER STEP IN US



//#define FULL_SPEED_DELAY  ONE_MINUTE_OF_MICROSECONDS / (DEFAULT_SEEKRATE * DEFAULT_X_STEPS_PER_MM)

#define FULL_SPEED_DELAY 60000000L / (480L*1260L)   // Approximately 99 microseconds at 480mm/sec

// On timer interrupt, check buttons and set next step pulse on the basis of the buttons.

uint32_t counter;

char st_process_manual_buttons(void)
{
	uint32_t delay = 1000;
	
	uint16_t max_rate;
	
	if (buttons[0] & buttons[1]){
		max_rate = max(2<<abs(buttons[0]), 2<<abs(buttons[1]));	
	} else {
		max_rate =0;
	}
	

	config_step_timer(delay);		// Takes microseconds between ticks

    out_bits = current_block->direction_bits;
    
  	out_bits=0;

  if (buttons[0] < 0) { out_bits |= (1<<X_DIRECTION_BIT); }
  if (buttons[1] < 0) { out_bits |= (1<<Y_DIRECTION_BIT); }
  if (buttons[2] < 0) { out_bits |= (1<<Z_DIRECTION_BIT); }


 
 if (abs(buttons[0])>0){
 	if (abs(buttons[1])>abs(buttons[0])){
// Need code to deal with different speeds on x and y simultaneously
// Will do this by only updating the slower one every nth frame, but
// too lazy to write it right now.
 		
 	}
 
 
 	delay = FULL_SPEED_DELAY << (8-abs(buttons[0]));
 	config_step_timer(delay);		// Takes microseconds between ticks

      out_bits |= (1<<X_STEP_BIT);
	  if (out_bits & (1<<X_DIRECTION_BIT)){
		actual_position[X_AXIS]-=1;
	  } else {
		actual_position[X_AXIS]+=1;
	  }

 } 	
 if (abs(buttons[1])>0){
 	delay = FULL_SPEED_DELAY << (8-abs(buttons[1]));
 	config_step_timer(delay);		// Takes microseconds between ticks

      out_bits |= (1<<Y_STEP_BIT);
	  if (out_bits & (1<<Y_DIRECTION_BIT)){
		actual_position[Y_AXIS]-=1;
	  } else {
		actual_position[Y_AXIS]+=1;
	  }
 } 	
 if (abs(buttons[2])>0){
 	delay = FULL_SPEED_DELAY << (8-abs(buttons[2]));
 	config_step_timer(delay);		// Takes microseconds between ticks

      out_bits |= (1<<Z_STEP_BIT);
	  if (out_bits & (1<<Z_DIRECTION_BIT)){
		actual_position[Z_AXIS]-=1;
	  } else {
		actual_position[Z_AXIS]+=1;
	  }
 } 	

// Update the actual position register based on
// steps about to be issued.

  out_bits ^= settings.invert_mask;
  return 1;	// always return true
}
