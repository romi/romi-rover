/*
  romi-rover

  Copyright (C) 2019 Sony Computer Science Laboratories
  Author(s) Peter Hanappe

  romi-rover is collection of applications for the Romi Rover.

  romi-rover is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see
  <http://www.gnu.org/licenses/>.

 */
#ifndef _OQUAM_CONFIG_H_
#define _OQUAM_CONFIG_H_

/*
                  Board        Encoders   Limits   
gShield           Uno          0          1
Ext. controller   Mega 2560    1          1
RAMPS             Mega 2560   

*/

#define USE_GSHIELD 1
#define USE_EXT_CONTROLLER 0

#if USE_GSHIELD
#include "gshield.h"
#endif

#if USE_EXT_CONTROLLER
#include "extctrlr.h"
#endif


//#define PRESCALING 8
//#define FREQUENCY_STEPPER 10000
//#define INTERRUPTS_PER_MILLISECOND 10

#endif // _OQUAM_CONFIG_H_
